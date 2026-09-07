#pragma once
#include <functional>
#include <stdexcept>
#include <string>
#include <curl/curl.h>
#include "JsonUtil.h"

class ChatCancelled : public std::runtime_error {
public: ChatCancelled() : std::runtime_error("Generation cancelled") {}
};

// SSE framing is independent of TCP chunks and supports CRLF, comments and
// multiline data fields. JSON/UTF-8 decoding happens only after a whole event.
class SseDecoder {
public:
    explicit SseDecoder(std::function<void(const std::string&)> onData) : onData_(std::move(onData)) {}
    void feed(const char* data, size_t size);
    void finish();
private:
    void line(std::string text);
    std::string pending_, data_;
    std::function<void(const std::string&)> onData_;
};

class ChatTransport {
public:
    using Delta = std::function<void(const std::string&)>;
    using Cancelled = std::function<bool()>;
    static json request(const std::string& url, const std::string& key, json payload,
                        bool stream, bool dashscope, const Delta& delta,
                        const Cancelled& cancelled);
};
