#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace http {
// A close-delimited HTTP response. Transport callbacks queue socket operations
// onto the connection's event loop; workers use this thread-safe interface.
class ResponseStream {
public:
    using Writer = std::function<void(const std::string&)>;
    ResponseStream(Writer write, std::function<void()> finish)
        : write_(std::move(write)), finish_(std::move(finish)) {}
    // 将业务事件封装成 SSE：event 指定类型，data 携带 JSON，末尾空行表示一个事件结束。
    // 锁保护事件写入与结束操作；流已关闭时返回 false，通知上层停止继续生成。
    bool send(const std::string& event, const std::string& json) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        write_("event: " + event + "\ndata: " + json + "\n\n");
        return true;
    }
    // 每条流只执行一次结束回调；实际连接关闭操作由 HttpServer 绑定。
    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_.exchange(true)) finish_();
    }
    // 网络断连时标记关闭，供工作线程的取消检查读取。
    void disconnect() { closed_ = true; }
    bool closed() const { return closed_; }
private:
    Writer write_;
    std::function<void()> finish_;
    std::atomic<bool> closed_{false};
    std::mutex mutex_;
};
}
