#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>

#include "HttpContext.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../router/Router.h"
#include "../session/SessionManager.h"
#include "../middleware/MiddlewareChain.h"
#include "../middleware/cors/CorsMiddleware.h"
#include "../ssl/SslConnection.h"
#include "../ssl/SslContext.h"

class HttpRequest;
class HttpResponse;

namespace http
{

class HttpServer : muduo::noncopyable
{
public:
    using HttpCallback = std::function<void (const http::HttpRequest&, http::HttpResponse*)>;
    
    // 构造函数
    HttpServer(int port,
               const std::string& name,
               bool useSSL = false,
               muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);
    
    void setThreadNum(int numThreads)
    {
        server_.setThreadNum(numThreads);
    }

    void start();

    muduo::net::EventLoop* getLoop() const 
    { 
        return server_.getLoop(); 
    }

    void setHttpCallback(const HttpCallback& cb)
    {
        httpCallback_ = cb;
    }

    // 注册静态路由处理器
    void Get(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kGet, path, cb);
    }
    
    // 注册静态路由处理器
    void Get(const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kGet, path, handler);
    }

    void Post(const std::string& path, const HttpCallback& cb)
    {
        router_.registerCallback(HttpRequest::kPost, path, cb);
    }

    void Post(const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.registerHandler(HttpRequest::kPost, path, handler);
    }

    // 注册动态路由处理器
    void addRoute(HttpRequest::Method method, const std::string& path, router::Router::HandlerPtr handler)
    {
        router_.addRegexHandler(method, path, handler);
    }

    // 注册动态路由处理函数
    void addRoute(HttpRequest::Method method, const std::string& path, const router::Router::HandlerCallback& callback)
    {
        router_.addRegexCallback(method, path, callback);
    }

    // 发送延迟响应：异步 handler 的工作线程完成后调用，按其 delayedId 找回连接并发送
    void sendDeferredResponse(const http::HttpResponse& response);

    // 设置会话管理器
    void setSessionManager(std::unique_ptr<session::SessionManager> manager)
    {
        sessionManager_ = std::move(manager);
    }

    // 获取会话管理器
    session::SessionManager* getSessionManager() const
    {
        return sessionManager_.get();
    }

    // 添加中间件的方法
    void addMiddleware(std::shared_ptr<middleware::Middleware> middleware) 
    {
        middlewareChain_.addMiddleware(middleware);
    }

    void enableSSL(bool enable) 
    {
        useSSL_ = enable;
    }

    void setSslConfig(const ssl::SslConfig& config);

private:
    void initialize();

    void onConnection(const muduo::net::TcpConnectionPtr& conn);
    void onMessage(const muduo::net::TcpConnectionPtr& conn,
                   muduo::net::Buffer* buf,
                   muduo::Timestamp receiveTime);
    void onRequest(const muduo::net::TcpConnectionPtr&, const HttpRequest&);

    void handleRequest(const HttpRequest& req, HttpResponse* resp);
    
private:
    muduo::net::InetAddress                      listenAddr_; // 监听地址
    muduo::net::EventLoop                        mainLoop_; // 主循环
    muduo::net::TcpServer                        server_;
    HttpCallback                                 httpCallback_; // 回调函数
    router::Router                               router_; // 路由
    std::unique_ptr<session::SessionManager>     sessionManager_; // 会话管理器
    middleware::MiddlewareChain                  middlewareChain_; // 中间件链
    std::unique_ptr<ssl::SslContext>             sslCtx_; // SSL 上下文
    bool                                         useSSL_; // 是否使用 SSL
    // TcpConnectionPtr -> SslConnectionPtr
    std::map<muduo::net::TcpConnectionPtr, std::unique_ptr<ssl::SslConnection>> sslConns_;

    // ---- 延迟响应支持 ----
    // 延迟响应条目：暂存的连接 + 中间件处理过的响应（保留 CORS 等响应头）
    struct DeferredEntry
    {
        muduo::net::TcpConnectionPtr conn;
        http::HttpResponse           resp;
    };
    std::mutex                deferredMutex_;          // 保护 deferredEntries_ 的互斥锁
    std::unordered_map<uint64_t, DeferredEntry> deferredEntries_; // 延迟响应 ID -> 连接与响应
    std::atomic<uint64_t>     nextDeferredId_{1};      // 延迟响应 ID 自增分配器（多 IO 线程安全）
    std::mutex streamsMutex_;
    std::unordered_map<std::string, std::weak_ptr<ResponseStream>> streams_;
}; 

} // namespace http
