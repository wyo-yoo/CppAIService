#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ChatTaskPool {
public:
    explicit ChatTaskPool(size_t workers = 4, size_t capacity = 32) : capacity_(capacity) {
        for (size_t i = 0; i < workers; ++i) workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                    if (tasks_.empty()) return;
                    task = std::move(tasks_.front()); tasks_.pop();
                }
                try { task(); } catch (...) { /* Each task owns its error response. */ }
            }
        });
    }
    bool submit(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= capacity_) return false;
        tasks_.push(std::move(task)); ready_.notify_one(); return true;
    }
    void shutdown() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        for (auto& thread : workers_) if (thread.joinable()) thread.join();
    }
    ~ChatTaskPool() { shutdown(); }
private:
    const size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
