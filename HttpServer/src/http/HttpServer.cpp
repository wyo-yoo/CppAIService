#include "../../include/http/HttpServer.h"

#include <any>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>

namespace http
{

// 默认http回应函数
void defaultHttpCallback(const HttpRequest &req, HttpResponse *resp)
{
    resp->setStatusLine(req.getVersion(), HttpResponse::k404NotFound, "Not Found");
    resp->setContentLength(0);
    resp->setCloseConnection(true);
}

HttpServer::HttpServer(int port,
                       const std::string &name,
                       bool useSSL,
                       muduo::net::TcpServer::Option option)
    : listenAddr_(std::getenv("CHAT_BIND_ADDRESS") ? std::getenv("CHAT_BIND_ADDRESS") : "127.0.0.1", port)
    , server_(&mainLoop_, listenAddr_, name, option)
    , useSSL_(useSSL)
    , httpCallback_(std::bind(&HttpServer::handleRequest, this, std::placeholders::_1, std::placeholders::_2))
{
    initialize();
}

// 服务器运行函数
void HttpServer::start()
{
    LOG_WARN << "HttpServer[" << server_.name() << "] starts listening on" << server_.ipPort();
    server_.start();
    mainLoop_.loop();
}

void HttpServer::initialize()
{
    // 设置回调函数
    server_.setConnectionCallback(
        std::bind(&HttpServer::onConnection, this, std::placeholders::_1));
    server_.setMessageCallback(
        std::bind(&HttpServer::onMessage, this,
                  std::placeholders::_1,
                  std::placeholders::_2,
                  std::placeholders::_3));
}

void HttpServer::setSslConfig(const ssl::SslConfig& config)
{
    if (useSSL_)
    {
        sslCtx_ = std::make_unique<ssl::SslContext>(config);
        if (!sslCtx_->initialize())
        {
            LOG_ERROR << "Failed to initialize SSL context";
            abort();
        }
    }
}

void HttpServer::onConnection(const muduo::net::TcpConnectionPtr& conn)
{
    if (conn->connected())
    {
        if (useSSL_)
        {
            auto sslConn = std::make_unique<ssl::SslConnection>(conn, sslCtx_.get());
            sslConn->setMessageCallback(
                std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
            sslConns_[conn] = std::move(sslConn);
            sslConns_[conn]->startHandshake();
        }
        conn->setContext(HttpContext());
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(deferredMutex_);
            for (auto it = deferredEntries_.begin(); it != deferredEntries_.end();)
                if (it->second.conn == conn) it = deferredEntries_.erase(it); else ++it;
        }
        {
            std::lock_guard<std::mutex> lock(streamsMutex_);
            auto it = streams_.find(conn->name());
            if (it != streams_.end()) {
                if (auto stream = it->second.lock()) stream->disconnect();
                streams_.erase(it);
            }
        }
        if (useSSL_)
        {
            sslConns_.erase(conn);
        }
    }
}

void HttpServer::onMessage(const muduo::net::TcpConnectionPtr &conn,
                           muduo::net::Buffer *buf,
                           muduo::Timestamp receiveTime)
{
    try
    {
        {
            std::lock_guard<std::mutex> lock(streamsMutex_);
            if (streams_.count(conn->name())) {
                // A close-delimited streaming connection cannot accept another response.
                buf->retrieveAll();
                conn->forceClose();
                return;
            }
        }
        // 这层判断只是代表是否支持ssl
        if (useSSL_)
        {
            LOG_INFO << "onMessage useSSL_ is true";
            // 1.查找对应的SSL连接
            auto it = sslConns_.find(conn);
            if (it != sslConns_.end())
            {
                LOG_INFO << "onMessage sslConns_ is not empty";
                // 2. SSL连接处理数据
                it->second->onRead(conn, buf, receiveTime);

                // 3. 如果 SSL 握手还未完成，直接返回
                if (!it->second->isHandshakeCompleted())
                {
                    LOG_INFO << "onMessage sslConns_ is not empty";
                    return;
                }

                // 4. 从SSL连接的解密缓冲区获取数据
                muduo::net::Buffer* decryptedBuf = it->second->getDecryptedBuffer();
                if (decryptedBuf->readableBytes() == 0)
                    return; // 没有解密后的数据

                // 5. 使用解密后的数据进行HTTP 处理
                buf = decryptedBuf; // 将 buf 指向解密后的数据
                LOG_INFO << "onMessage decryptedBuf is not empty";
            }
        }
        // HttpContext对象用于解析出buf中的请求报文，并把报文的关键信息封装到HttpRequest对象中
        HttpContext *context = boost::any_cast<HttpContext>(conn->getMutableContext());
        if (!context->parseRequest(buf, receiveTime)) // 解析一个http请求
        {
            // 如果解析http报文过程中出错
            conn->send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            conn->forceCloseWithDelay(0.1); return;
        }
        // 如果buf缓冲区中解析出一个完整的数据包才封装响应报文
        if (context->gotAll())
        {
            onRequest(conn, context->request());
            context->reset();
        }
    }
    catch (const std::exception &e)
    {
        // 捕获异常，返回错误信息
        LOG_ERROR << "Exception in onMessage: " << e.what();
        conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
        conn->shutdown();
    }
}

void HttpServer::onRequest(const muduo::net::TcpConnectionPtr &conn, const HttpRequest &req)
{
    const std::string &connection = req.getHeader("Connection");
    bool close = ((connection == "close") ||
                  (req.getVersion() == "HTTP/1.0" && connection != "Keep-Alive"));
    HttpResponse response(close);

    // 为响应分配延迟 ID（异步 handler 用它找回连接；同步请求不用则忽略）
    response.setDeferredId(nextDeferredId_.fetch_add(1));

    // 根据请求报文信息来封装响应报文对象
    HttpRequest clientRequest = req;
    clientRequest.setPeerIp(conn->peerAddress().toIp());
    httpCallback_(clientRequest, &response); // 执行onHttpCallback函数

    // 流式响应先发响应头，再由业务回调持续写事件；这里不等待模型生成完整正文。
    if (response.streamHandler()) {
        std::weak_ptr<muduo::net::TcpConnection> weakConn = conn;
        // 将业务层的 send/finish 绑定到 TCP 连接；弱引用避免生成任务延长已断开连接的生命周期。
        auto stream = std::make_shared<ResponseStream>(
            [weakConn](const std::string& bytes) {
                if (auto connection = weakConn.lock()) connection->send(bytes);
            },
            [weakConn]() {
                if (auto connection = weakConn.lock()) connection->shutdown();
            });
        {
            std::lock_guard<std::mutex> lock(streamsMutex_);
            streams_[conn->name()] = stream;
        }
        std::weak_ptr<ResponseStream> weakStream = stream;
        // 慢客户端导致待发送缓冲超过阈值时，关闭流与连接，上层取消检查会终止模型请求。
        conn->setHighWaterMarkCallback([weakStream](const muduo::net::TcpConnectionPtr& connection, size_t) {
            if (auto output = weakStream.lock()) output->disconnect();
            connection->forceClose();
        }, 1024 * 1024);
        muduo::net::Buffer headers;
        response.appendToBuffer(&headers);
        conn->send(&headers);
        // Start only after response headers and disconnect tracking are ready.
        // 响应头与断连跟踪已就绪，此时才启动 ChatFeatures 注册的工作任务。
        try { response.streamHandler()(stream); }
        catch (...) { stream->finish(); }
        return;
    }

    // 延迟响应：handler 标记了异步处理，暂存连接与响应（含中间件添加的头），
    // 等工作线程完成后由 sendDeferredResponse 发送，此处直接返回不阻塞网络线程
    if (response.deferred())
    {
        {
            std::lock_guard<std::mutex> lock(deferredMutex_);
            deferredEntries_[response.deferredId()] = DeferredEntry{conn, response};
        }
        // Start only after the connection has been registered; fast jobs cannot lose their response.
        if (response.deferredHandler()) response.deferredHandler()();
        return;
    }

    // 可以给response设置一个成员，判断是否请求的是文件，如果是文件设置为true，并且存在文件位置在这里send出去。
    muduo::net::Buffer buf;
    response.appendToBuffer(&buf);

    conn->send(&buf);
    // 如果是短连接的话，返回响应报文后就断开连接
    if (response.closeConnection())
    {
        conn->shutdown();
    }
}

// 发送延迟响应：异步 handler 的工作线程完成后调用（线程安全：muduo 连接支持跨线程发送）
void HttpServer::sendDeferredResponse(const http::HttpResponse &response)
{
    DeferredEntry entry;
    {
        std::lock_guard<std::mutex> lock(deferredMutex_);
        auto it = deferredEntries_.find(response.deferredId());
        if (it == deferredEntries_.end())
        {
            LOG_WARN << "sendDeferredResponse: deferred id not found: " << response.deferredId();
            return;
        }
        entry = std::move(it->second);
        deferredEntries_.erase(it);
    }

    // 以中间件处理过的响应为基础（保留 CORS 等响应头），合并工作线程填充的状态与内容
    HttpResponse finalResp = entry.resp;
    finalResp.mergeFrom(response);

    muduo::net::Buffer buf;
    finalResp.appendToBuffer(&buf);

    // 跨线程发送（muduo 保证线程安全）；连接可能已关闭，muduo 内部会安全处理
    entry.conn->send(&buf);
    if (finalResp.closeConnection())
    {
        entry.conn->shutdown();
    }
}

// 执行请求对应的路由处理函数
void HttpServer::handleRequest(const HttpRequest &req, HttpResponse *resp)
{
    try
    {
        // 处理请求前的中间件
        HttpRequest mutableReq = req;
        middlewareChain_.processBefore(mutableReq);

        // 路由处理
        if (!router_.route(mutableReq, resp))
        {
            LOG_INFO << "请求的啥，url：" << req.method() << " " << req.path();
            LOG_INFO << "未找到路由，返回404";
            defaultHttpCallback(mutableReq, resp);
        }

        // 处理响应后的中间件
        middlewareChain_.processAfter(*resp);
    }
    catch (const HttpResponse& res) 
    {
        // 处理中间件抛出的响应（如CORS预检请求）
        *resp = res;
    }
    catch (const std::exception& e) 
    {
        // 错误处理
        LOG_ERROR << "Request handler failed";
        resp->setStatusLine(req.getVersion(), HttpResponse::k500InternalServerError, "Internal Server Error");
        resp->setContentType("application/json");
        const std::string body = "{\"success\":false,\"message\":\"Service temporarily unavailable\"}";
        resp->setBody(body); resp->setContentLength(body.size()); resp->setCloseConnection(true);
    }
}

} // namespace http
