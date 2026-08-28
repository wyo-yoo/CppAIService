#pragma once

#include <cstdint>
#include <map>
#include <string>

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
};

} // namespace http