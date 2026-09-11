#include "AIUtil/AIStrategy.h"
#include <cstdlib>
#include <stdexcept>

DeepSeekStrategy::DeepSeekStrategy() {
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    if (!key || !*key) throw std::runtime_error("DEEPSEEK_API_KEY is not configured");
    apiKey_ = key;
    const char* model = std::getenv("DEEPSEEK_MODEL");
    model_ = model && *model ? model : "deepseek-flash";
}

std::string DeepSeekStrategy::getApiUrl() const {
    return "https://api.deepseek.com/chat/completions";
}

std::string DeepSeekStrategy::getApiKey() const { return apiKey_; }
std::string DeepSeekStrategy::getModel() const { return model_; }

json DeepSeekStrategy::buildRequest(
    const std::vector<std::pair<std::string, long long>>& messages) const {
    json history = json::array();
    for (size_t i = 0; i < messages.size(); ++i) {
        history.push_back({{"role", i % 2 == 0 ? "user" : "assistant"},
                           {"content", messages[i].first}});
    }
    // Standard chat returns answer text directly through the existing SSE path.
    return {{"model", model_}, {"messages", history}, {"thinking", {{"type", "disabled"}}}};
}

std::string DeepSeekStrategy::parseResponse(const json& response) const {
    const auto& content = response.at("choices").at(0).at("message").at("content");
    if (!content.is_string() || content.get_ref<const std::string&>().empty()) {
        throw std::runtime_error("DeepSeek returned no answer text");
    }
    return content.get<std::string>();
}

std::string DeepSeekStrategy::chat(
    std::vector<std::pair<std::string, long long>>& messages,
    const std::string& userQuestion,
    std::function<json(const json&, bool)> httpExecutor) {
    messages.emplace_back(userQuestion, nowMs());
    const auto answer = parseResponse(httpExecutor(buildRequest(messages), true));
    messages.emplace_back(answer, nowMs());
    return answer;
}
