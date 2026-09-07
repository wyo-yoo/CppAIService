#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "AIFactory.h"
#include "ChatStream.h"

class AIHelper {
public:
    using Messages = std::vector<std::pair<std::string, long long>>;
    using Persist = std::function<void(int, const std::string&, bool, const std::string&, long long, const std::string&)>;
    struct Result { std::string text; bool stopped = false; };
    explicit AIHelper(Persist persist = {}) : persist_(std::move(persist)) {}
    void setStrategy(std::shared_ptr<AIStrategy> strategy);
    void addMessage(int userId, const std::string& name, bool isUser, const std::string& text, std::string sessionId);
    void restoreMessage(const std::string& text, long long timestamp);
    std::string chat(int userId, std::string name, std::string sessionId, std::string question, std::string model);
    Result chatStream(int userId, const std::string& name, const std::string& sessionId,
                      const std::string& question, const std::string& model,
                      const ChatTransport::Delta& delta, const ChatTransport::Cancelled& cancelled);
    Messages GetMessages();
    json request(const json& payload);
private:
    void pushMessageToMysql(int userId, const std::string& name, bool isUser,
                            const std::string& text, long long timestamp, const std::string& sessionId);
    Messages messages_;
    std::mutex messagesMutex_, generationMutex_;
    std::shared_ptr<AIStrategy> strategy_;
    Persist persist_;
};
