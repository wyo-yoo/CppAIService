#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <functional>
#include "ResponseStream.h"

#include <muduo/net/TcpServer.h>

namespace http
{

class HttpResponse 
{
public:
    enum HttpStatusCode
    {
        kUnknown,
        k200Ok = 200,
        k204NoContent = 204,
        k301MovedPermanently = 301,
        k400BadRequest = 400,
        k401Unauthorized = 401,
        k403Forbidden = 403,
        k404NotFound = 404,
        k409Conflict = 409,
        k500InternalServerError = 500,
    };

    HttpResponse(bool close = true)
        : statusCode_(kUnknown)
        , closeConnection_(close)
        , deferred_(false)
        , deferredId_(0)
    {}

    void setVersion(std::string version)
    { httpVersion_ = version; }
    void setStatusCode(HttpStatusCode code)
    { statusCode_ = code; }

    HttpStatusCode getStatusCode() const
    { return statusCode_; }

    void setStatusMessage(const std::string message)
    { statusMessage_ = message; }

    void setCloseConnection(bool on)
    { closeConnection_ = on; }

    bool closeConnection() const
    { return closeConnection_; }
    
    void setContentType(const std::string& contentType)
    { addHeader("Content-Type", contentType); }

    void setContentLength(uint64_t length)
    { addHeader("Content-Length", std::to_string(length)); }

    void addHeader(const std::string& key, const std::string& value)
    { headers_[key] = value; }
    
    void setBody(const std::string& body)
    { 
        body_ = body;
        // body_ += "\0";
    }

    void setStatusLine(const std::string& version,
                         HttpStatusCode statusCode,
                         const std::string& statusMessage);

    // ---- 延迟响应支持：供异步 handler 使用 ----
    // 标记为延迟响应：网络线程跳过发送，由工作线程完成后调用 HttpServer::sendDeferredResponse 发送
    void setDeferredHandler(std::function<void()> handler) { deferred_ = true; deferredHandler_ = std::move(handler); }
    const std::function<void()>& deferredHandler() const { return deferredHandler_; }
    void setDeferred(bool on)
    { deferred_ = on; }
    bool deferred() const
    { return deferred_; }

    // 延迟响应 ID：由 HttpServer 分配，用于在工作线程中找回对应连接
    void setDeferredId(uint64_t id)
    { deferredId_ = id; }
    uint64_t deferredId() const
    { return deferredId_; }

    // 把另一个响应的状态与内容合并到本响应（保留本响应已有的额外头，如中间件加的 CORS 头）
    void mergeFrom(const HttpResponse& other)
    {
        httpVersion_ = other.httpVersion_;
        statusCode_ = other.statusCode_;
        statusMessage_ = other.statusMessage_;
        closeConnection_ = other.closeConnection_;
        body_ = other.body_;
        for (const auto& kv : other.headers_)
        {
            headers_[kv.first] = kv.second;
        }
    }

    void setErrorHeader(){}

    using StreamHandler = std::function<void(std::shared_ptr<ResponseStream>)>;
    // 回答长度尚未知，移除 Content-Length，以关闭连接标记响应结束；正文格式为 SSE。
    void setStreamHandler(StreamHandler handler) {
        streamHandler_ = std::move(handler);
        closeConnection_ = true;
        headers_.erase("Content-Length");
        headers_["Content-Type"] = "text/event-stream; charset=utf-8";
        // 提示客户端及代理不要缓存或转换响应，并通知支持此头的 Nginx 禁用缓冲，及时转发片段。
        headers_["Cache-Control"] = "no-cache, no-transform";
        headers_["X-Accel-Buffering"] = "no";
    }
    const StreamHandler& streamHandler() const { return streamHandler_; }

    void appendToBuffer(muduo::net::Buffer* outputBuf) const;
private:
    std::string                        httpVersion_; 
    HttpStatusCode                     statusCode_;
    std::string                        statusMessage_;
    bool                               closeConnection_;
    std::map<std::string, std::string> headers_;
    std::string                        body_;
    bool                               isFile_;
    bool                               deferred_;   // 是否为延迟响应（异步 handler 完成后发送）
    uint64_t                           deferredId_; // 延迟响应 ID（对应 HttpServer 中暂存的连接）
    StreamHandler streamHandler_;
    std::function<void()> deferredHandler_;
};

} // namespace http
