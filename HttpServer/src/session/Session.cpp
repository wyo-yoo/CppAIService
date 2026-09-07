#include "../include/session/Session.h"

#include "../include/session/SessionManager.h"

namespace http
{
namespace session
{

Session::Session(const std::string& sessionId, SessionManager* sessionManager, int maxAge)
    : sessionId_(sessionId)
    , maxAge_(maxAge)
    , sessionManager_(sessionManager)
{
    refresh(); // 初始化时设置过期时间
}

// 检查会话是否已过期
bool Session::isExpired() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::chrono::system_clock::now() > expiryTime_;
}

// 刷新会话的过期时间
void Session::refresh()
{
    std::lock_guard<std::mutex> lock(mutex_);
    expiryTime_ = std::chrono::system_clock::now() + std::chrono::seconds(maxAge_);
}

// 设置会话数据
void Session::setValue(const std::string& key, const std::string& value)
{
    { std::lock_guard<std::mutex> lock(mutex_); data_[key] = value; }
    // 如果设置了manager，自动保存更改
    auto manager = getManager();
    if (manager)
    {
        manager->updateSession(shared_from_this());
    }
}

// 获取会话数据
std::string Session::getValue(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    return it != data_.end() ? it->second : std::string();
}

// 删除会话数据
void Session::remove(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.erase(key);
}

// 清空会话数据
void Session::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_.clear();
}

} // namespace session
} // namespace http
