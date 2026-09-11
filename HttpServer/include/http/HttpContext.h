#pragma once

#include <iostream>

#include <muduo/net/TcpServer.h>

#include "HttpRequest.h"

namespace http
{

class HttpContext
{
public:
    // 请求大小限制，防止慢速攻击和大包 DoS
    static constexpr size_t kMaxRequestLineLength = 8 * 1024;      // 请求行上限 8KB
    static constexpr size_t kMaxHeadersTotalSize  = 8 * 1024;      // 请求头总大小上限 8KB
    static constexpr size_t kMaxBodySize          = 64 * 1024; // Chat JSON requests are bounded to 64 KiB

    enum HttpRequestParseState
    {
        kExpectRequestLine, // 解析请求行
        kExpectHeaders, // 解析请求头
        kExpectBody, // 解析请求体
        kGotAll, // 解析完成
    };
    
    HttpContext()
    : state_(kExpectRequestLine)
    {}

    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);
    bool gotAll() const 
    { return state_ == kGotAll;  }

    void reset()
    {
        state_ = kExpectRequestLine;
        headersParsedSize_ = 0;
        HttpRequest dummyData;
        request_.swap(dummyData);
    }

    const HttpRequest& request() const
    { return request_;}

    HttpRequest& request()
    { return request_;}

private:
    bool processRequestLine(const char* begin, const char* end);
private:
    HttpRequestParseState state_;
    HttpRequest           request_;
    size_t                headersParsedSize_{0}; // header 已解析字节累计
};

} // namespace http