#include "AIUtil/ChatStream.h"
#include <memory>
#include <mutex>

// 模型接口的 SSE 事件以空行结束；同一事件的多行 data: 先合并，再交给 JSON 解析回调。
void SseDecoder::line(std::string text) {
    if (!text.empty() && text.back() == '\r') text.pop_back();
    if (text.empty()) {
        if (!data_.empty()) {
            data_.pop_back();
            auto event = std::move(data_);
            data_.clear();
            onData_(event);
        }
    } else if (text.compare(0, 5, "data:") == 0) {
        size_t offset = text.size() > 5 && text[5] == ' ' ? 6 : 5;
        data_ += text.substr(offset) + '\n';
    }
}
// 一次网络回调可能只有半行，也可能包含多个事件；保留末尾残片，等下次数据到达后继续拼接。
void SseDecoder::feed(const char* data, size_t size) {
    pending_.append(data, size);
    if (pending_.size() + data_.size() > 1024 * 1024) throw std::runtime_error("Upstream event too large");
    size_t start = 0, end;
    while ((end = pending_.find('\n', start)) != std::string::npos) {
        line(pending_.substr(start, end - start));
        start = end + 1;
    }
    pending_.erase(0, start);
}
void SseDecoder::finish() {
    if (!pending_.empty()) { line(std::move(pending_)); pending_.clear(); }
    line("");
}

// 流式接收链路：curl 字节回调 -> SseDecoder -> 提取回答片段 -> delta 回调 -> 浏览器。
// 同时累计完整 answer，供模型策略解析、会话保存和最终 done 事件使用。
json ChatTransport::request(const std::string& url, const std::string& key, json payload,
                            bool stream, bool dashscope, const Delta& delta,
                            const Cancelled& cancelled) {
    static std::once_flag init;
    std::call_once(init, [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("CURL initialization failed"); });
    if (cancelled && cancelled()) throw ChatCancelled();
    // 普通聊天接口使用 stream=true；DashScope 应用接口通过增量参数和下面的专用请求头启用 SSE。
    if (stream) {
        if (dashscope) payload["parameters"]["incremental_output"] = true;
        else payload["stream"] = true;
    }
    std::string answer, raw;
    std::exception_ptr callbackError;
    bool completed = false;
    // 此时已拼出一个完整 SSE 事件，才解析 JSON，避免把 TCP 分片误当成完整消息。
    SseDecoder decoder([&](const std::string& text) {
        // 记录模型的正常结束标志；单纯连接关闭不能证明回答已完整生成。
        if (text == "[DONE]") { completed = true; return; }
        auto value = json::parse(text);
        if (value.contains("error") || value.contains("code")) throw std::runtime_error("Upstream rejected the model request");
        std::string piece;
        if (dashscope && value.contains("output")) {
            piece = value["output"].value("text", "");
            if (value["output"].value("finish_reason", "") == "stop") completed = true;
        } else if (value.contains("choices") && !value["choices"].empty()) {
            const auto& choice = value["choices"][0];
            if (choice.contains("delta") && choice["delta"].contains("content") && choice["delta"]["content"].is_string()) piece = choice["delta"]["content"];
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) completed = true;
        }
        // piece 是本次新增内容；累计全文的同时立即向业务层转发，不等待整段回答完成。
        answer += piece;
        if (answer.size() > 8 * 1024 * 1024) throw std::runtime_error("Model answer too large");
        if (!piece.empty() && delta) delta(piece);
    });
    struct State { SseDecoder* decoder; std::string* raw; std::exception_ptr* error; bool stream; } state{&decoder, &raw, &callbackError, stream};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!curl || !multi) throw std::runtime_error("Cannot allocate HTTP client");
    curl_slist* list = nullptr;
    list = curl_slist_append(list, ("Authorization: Bearer " + key).c_str());
    list = curl_slist_append(list, "Content-Type: application/json");
    if (stream) list = curl_slist_append(list, "Accept: text/event-stream");
    if (stream && dashscope) list = curl_slist_append(list, "X-DashScope-SSE: enable");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
    const auto body = payload.dump();
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
    // libcurl 每收到一批响应字节就调用这里：流式模式增量解码，普通模式先收齐 JSON。
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, +[](char* bytes, size_t size, size_t count, void* pointer) -> size_t {
        auto& state = *static_cast<State*>(pointer);
        size_t length = size * count;
        try {
            if (state.stream) state.decoder->feed(bytes, length);
            else {
                state.raw->append(bytes, length);
                if (state.raw->size() > 8 * 1024 * 1024) throw std::runtime_error("Upstream response too large");
            }
            return length;
        // 异常不能穿过 curl 的 C 回调边界；先保存异常并返回 0 中止传输，退出后再抛出。
        } catch (...) { *state.error = std::current_exception(); return 0; }
    });
    if (curl_multi_add_handle(multi.get(), curl.get()) != CURLM_OK) throw std::runtime_error("Cannot start model request");
    struct Detach { CURLM* multi; CURL* easy; ~Detach() { curl_multi_remove_handle(multi, easy); } } detach{multi.get(), curl.get()};
    // 在聊天工作线程中驱动 curl multi，轮询间隙检查取消标记；网络线程无需等待模型。
    int running = 0;
    do {
        if (cancelled && cancelled()) throw ChatCancelled();
        if (curl_multi_perform(multi.get(), &running) != CURLM_OK) throw std::runtime_error("Model transport failed");
        if (running && curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr) != CURLM_OK) throw std::runtime_error("Model transport failed");
    } while (running);
    if (cancelled && cancelled()) throw ChatCancelled();
    if (callbackError) std::rethrow_exception(callbackError);
    int left;
    CURLcode result = CURLE_OK;
    while (auto message = curl_multi_info_read(multi.get(), &left)) {
        if (message->msg == CURLMSG_DONE) result = message->data.result;
    }
    if (result != CURLE_OK) throw std::runtime_error(std::string("Model request failed: ") + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) throw std::runtime_error("Model service returned HTTP " + std::to_string(status));
    if (!stream) {
        auto result = json::parse(raw);
        if (result.contains("error") || result.contains("code")) throw std::runtime_error("Model service rejected the request");
        return result;
    }
    // 处理最后残留的数据，并确认收到结束标志；完整结果仍按策略原有的 JSON 结构返回。
    decoder.finish();
    if (!completed) throw std::runtime_error("Model stream ended before completion");
    return dashscope ? json{{"output", {{"text", answer}}}} : json{{"choices", json::array({{{"message", {{"content", answer}}}}})}};
}
