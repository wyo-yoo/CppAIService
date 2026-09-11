#include "../include/session/SessionStorage.h"
#include <iostream>
#include <stdexcept>

namespace http
{

namespace session
{

void MemorySessionStorage::save(std::shared_ptr<Session> session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (session->getId().empty()) return;
    for (auto it = sessions_.begin(); it != sessions_.end();)
        if (it->second->isExpired()) it = sessions_.erase(it); else ++it;
    if (sessions_.size() >= 10000 && !sessions_.count(session->getId())) throw std::runtime_error("Session capacity reached");
    sessions_[session->getId()] = session;
}

// 通过会话ID从存储中加载会话
std::shared_ptr<Session> MemorySessionStorage::load(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(sessionId);
    if (it != sessions_.end())
    {
        if (!it->second->isExpired())
        {
            return it->second;
        }
        else
        {
            // 如果会话已过期，则从存储中移除
            sessions_.erase(it);
        }
    }

    // 如果会话不存在或已过期，则返回nullptr
    return nullptr;
}

// 通过会话ID从存储中移除会话
void MemorySessionStorage::remove(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(sessionId);
}

} // namespace session
} // namespace http
