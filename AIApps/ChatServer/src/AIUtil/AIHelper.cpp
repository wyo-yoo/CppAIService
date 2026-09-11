#include "AIUtil/AIHelper.h"
#include "AIUtil/MQManager.h"
#include <algorithm>
#include <chrono>
#include "security/PublicAccess.h"

namespace {
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}
void AIHelper::setStrategy(std::shared_ptr<AIStrategy> strategy) {
    std::lock_guard<std::mutex> lock(messagesMutex_); strategy_ = std::move(strategy);
}
void AIHelper::restoreMessage(const std::string& text, long long timestamp) {
    std::lock_guard<std::mutex> lock(messagesMutex_); messages_.emplace_back(text, timestamp);
}
void AIHelper::addMessage(int userId, const std::string& name, bool isUser, const std::string& text, std::string sessionId) {
    auto timestamp = nowMs(); restoreMessage(text, timestamp);
    pushMessageToMysql(userId, name, isUser, text, timestamp, sessionId);
}
AIHelper::Messages AIHelper::GetMessages() {
    std::lock_guard<std::mutex> lock(messagesMutex_); return messages_;
}
json AIHelper::request(const json& payload) {
    std::shared_ptr<AIStrategy> strategy;
    { std::lock_guard<std::mutex> lock(messagesMutex_); strategy = strategy_; }
    if (!strategy) strategy = StrategyFactory::instance().create("1");
    return ChatTransport::request(strategy->getApiUrl(), strategy->getApiKey(), payload, false, false, {}, {});
}
std::string AIHelper::chat(int userId, std::string name, std::string sessionId, std::string question, std::string model) {
    return chatStream(userId, name, sessionId, question, model, {}, {}).text;
}
// 串起会话历史、模型策略和流式传输；delta 负责实时显示，partial 负责保留已生成的文本。
AIHelper::Result AIHelper::chatStream(int userId, const std::string& name, const std::string& sessionId,
                                     const std::string& question, const std::string& model,
                                     const ChatTransport::Delta& delta, const ChatTransport::Cancelled& cancelled) {
    // 同一会话只允许一个生成任务，防止两次回答的上下文和输出互相交叉。
    std::unique_lock<std::mutex> generation(generationMutex_, std::try_to_lock);
    if (!generation) throw std::runtime_error("This conversation is already generating a reply");
    auto strategy = StrategyFactory::instance().create(model);
    auto snapshot = GetMessages();
    // Send only the most recent complete turns, bounded by both count and bytes.
    size_t bytes = question.size();
    size_t start = snapshot.size();
    while (start >= 2 && snapshot.size() - start < 20) {
        size_t pairBytes = snapshot[start-2].first.size() + snapshot[start-1].first.size();
        if (bytes + pairBytes > 32768) break;
        bytes += pairBytes; start -= 2;
    }
    snapshot.erase(snapshot.begin(),snapshot.begin()+start);
    const long long userTime = std::max(nowMs(), snapshot.empty() ? 0LL : snapshot.back().second + 1);
    Result result;
    std::string partial;
    try {
        if (cancelled && cancelled()) throw ChatCancelled();
        // 只有提供 delta 回调且策略声明为最终回答时才启用流式；MCP 的工具判断阶段先收完整 JSON。
        result.text = strategy->chat(snapshot, question, [&](const json& payload, bool finalAnswer) {
            auto bounded = payload;
            const int maximum = settingInt("CHAT_MAX_OUTPUT_TOKENS",1024,64,8192);
            if (model == "3") bounded["parameters"]["max_tokens"] = maximum;
            else bounded["max_tokens"] = maximum;
            return ChatTransport::request(strategy->getApiUrl(), strategy->getApiKey(), bounded,
                static_cast<bool>(delta) && finalAnswer, model == "3",
                // 每个片段先记入 partial，再交给 ChatFeatures 中的回调发送 SSE。
                [&](const std::string& piece) { partial += piece; if (delta) delta(piece); }, cancelled);
        });
        // 如 MCP 首轮直接回答、无需调用工具，就把该完整结果作为一个 delta 发给前端。
        if (partial.empty() && delta && !result.text.empty()) delta(result.text);
    } catch (const ChatCancelled&) {
        // 主动取消或断连时，保留已生成内容并标记停止，而不是丢弃整次回答。
        result.text = partial; result.stopped = true;
    }
    // On other errors the private copy is discarded, preserving role order.
    // 正常结束或取消后，成对保存问题与回答；消息持久化在生成结束后进行，不按片段重复入库。
    const long long assistantTime = std::max(nowMs(), userTime + 1);
    {
        std::lock_guard<std::mutex> lock(messagesMutex_);
        messages_.emplace_back(question, userTime);
        messages_.emplace_back(result.text, assistantTime);
    }
    pushMessageToMysql(userId, name, true, question, userTime, sessionId);
    pushMessageToMysql(userId, name, false, result.text, assistantTime, sessionId);
    return result;
}
void AIHelper::pushMessageToMysql(int userId, const std::string& name, bool isUser,
                                 const std::string& text, long long timestamp, const std::string& sessionId) {
    if (persist_) { persist_(userId, name, isUser, text, timestamp, sessionId); return; }
    json message{{"type", "chat_message_v2"}, {"userId", userId}, {"username", name},
                 {"sessionId", sessionId}, {"isUser", isUser}, {"content", text}, {"timestamp", timestamp}};
    MQManager::instance().publish("sql_queue", message.dump());
}
