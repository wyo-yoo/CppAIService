#include"../include/AIUtil/AIStrategy.h"
#include"../include/AIUtil/AIFactory.h"
#include"../include/AIUtil/AIConfig.h"
#include"../include/AIUtil/AIToolRegistry.h"

// ========== AliyunStrategy ==========

std::string AliyunStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string AliyunStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}

std::string AliyunStrategy::chat(
    std::vector<std::pair<std::string, long long>>& messages,
    const std::string& userQuestion,
    std::function<json(const json&)> httpExecutor)
{
    messages.push_back({userQuestion, nowMs()});
    json payload = buildRequest(messages);
    json response = httpExecutor(payload);
    std::string answer = parseResponse(response);
    messages.push_back({answer, nowMs()});
    return answer.empty() ? "[Error] 无法解析响应" : answer;
}


// ========== DouBaoStrategy ==========

std::string DouBaoStrategy::getApiUrl()const {
    return "https://ark.cn-beijing.volces.com/api/v3/chat/completions";
}

std::string DouBaoStrategy::getApiKey()const {
    return apiKey_;
}


std::string DouBaoStrategy::getModel() const {
    return "doubao-seed-1-6-thinking-250715";
}


json DouBaoStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string DouBaoStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}

std::string DouBaoStrategy::chat(
    std::vector<std::pair<std::string, long long>>& messages,
    const std::string& userQuestion,
    std::function<json(const json&)> httpExecutor)
{
    messages.push_back({userQuestion, nowMs()});
    json payload = buildRequest(messages);
    json response = httpExecutor(payload);
    std::string answer = parseResponse(response);
    messages.push_back({answer, nowMs()});
    return answer.empty() ? "[Error] 无法解析响应" : answer;
}


// ========== AliyunRAGStrategy ==========

std::string AliyunRAGStrategy::getApiUrl() const {
    const char* key = std::getenv("Knowledge_Base_ID");
    if (!key) throw std::runtime_error("Knowledge_Base_ID not found!");
    std::string id(key);
    //拼接对应知识库ID
    return "https://dashscope.aliyuncs.com/api/v1/apps/"+id+"/completion";
}

std::string AliyunRAGStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunRAGStrategy::getModel() const {
    return ""; //不需要模型
}


json AliyunRAGStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    json msgArray = json::array();
    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        msg["role"] = (i % 2 == 0 ? "user" : "assistant");
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["input"]["messages"] = msgArray;
    payload["parameters"] = json::object();
    return payload;
}


std::string AliyunRAGStrategy::parseResponse(const json& response) const {
    if (response.contains("output") && response["output"].contains("text")) {
        return response["output"]["text"];
    }
    return {};
}

std::string AliyunRAGStrategy::chat(
    std::vector<std::pair<std::string, long long>>& messages,
    const std::string& userQuestion,
    std::function<json(const json&)> httpExecutor)
{
    messages.push_back({userQuestion, nowMs()});
    json payload = buildRequest(messages);
    json response = httpExecutor(payload);
    std::string answer = parseResponse(response);
    messages.push_back({answer, nowMs()});
    return answer.empty() ? "[Error] 无法解析响应" : answer;
}


// ========== AliyunMcpStrategy（MCP 两段式推理）==========

std::string AliyunMcpStrategy::getApiUrl() const {
    return "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
}

std::string AliyunMcpStrategy::getApiKey()const {
    return apiKey_;
}


std::string AliyunMcpStrategy::getModel() const {
    return "qwen-plus";
}


json AliyunMcpStrategy::buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const {
    json payload;
    payload["model"] = getModel();
    json msgArray = json::array();

    for (size_t i = 0; i < messages.size(); ++i) {
        json msg;
        if (i % 2 == 0) {
            msg["role"] = "user";
        }
        else {
            msg["role"] = "assistant";
        }
        msg["content"] = messages[i].first;
        msgArray.push_back(msg);
    }
    payload["messages"] = msgArray;
    return payload;
}


std::string AliyunMcpStrategy::parseResponse(const json& response) const {
    if (response.contains("choices") && !response["choices"].empty()) {
        return response["choices"][0]["message"]["content"];
    }
    return {};
}

std::string AliyunMcpStrategy::chat(
    std::vector<std::pair<std::string, long long>>& messages,
    const std::string& userQuestion,
    std::function<json(const json&)> httpExecutor)
{
    // 加载 MCP 配置（prompt 模板 + 工具列表）
    AIConfig config;
    config.loadFromFile("../AIApps/ChatServer/resource/config.json");

    // 第一次调用：注入 prompt，让模型判断是否需要工具
    std::string tempUserQuestion = config.buildPrompt(userQuestion);
    std::cout << "tempUserQuestion is " << tempUserQuestion << std::endl;
    messages.push_back({tempUserQuestion, 0});

    json firstReq = buildRequest(messages);
    json firstResp = httpExecutor(firstReq);
    std::string aiResult = parseResponse(firstResp);
    // 用完立即移除提示词
    messages.pop_back();

    std::cout << "aiResult is " << aiResult << std::endl;

    // 解析AI响应（是否工具调用）
    AIToolCall call = config.parseAIResponse(aiResult);

    // 情况1：AI 不调用工具
    if (!call.isToolCall) {
        messages.push_back({userQuestion, nowMs()});
        messages.push_back({aiResult, nowMs()});
        std::cout << "No tools required" << std::endl;
        return aiResult;
    }

    // 情况 2：AI 要调用工具
    json toolResult;
    AIToolRegistry registry;

    try {
        toolResult = registry.invoke(call.toolName, call.args);
        std::cout << "Tool call success" << std::endl;
    }
    catch (const std::exception& e) {
        std::string err = "[工具调用失败] " + std::string(e.what());
        messages.push_back({userQuestion, nowMs()});
        messages.push_back({err, nowMs()});
        std::cout << "Tool call failed" << std::endl << std::string(e.what());
        return err;
    }

    // 第二次调用：用同样的 prompt_template，但说明工具执行过
    std::string secondPrompt = config.buildToolResultPrompt(
        userQuestion, call.toolName, call.args, toolResult);
    std::cout << "secondPrompt is " << secondPrompt << std::endl;
    messages.push_back({secondPrompt, 0});

    json secondReq = buildRequest(messages);
    json secondResp = httpExecutor(secondReq);
    std::string finalAnswer = parseResponse(secondResp);
    // 删除包含提示词的信息
    messages.pop_back();

    std::cout << "finalAnswer is " << finalAnswer << std::endl;

    messages.push_back({userQuestion, nowMs()});
    messages.push_back({finalAnswer, nowMs()});
    return finalAnswer;
}


// ========== 静态注册（利用全局变量自动注册到工厂）==========

static StrategyRegister<AliyunStrategy> regAliyun("1");
static StrategyRegister<DouBaoStrategy> regDoubao("2");
static StrategyRegister<AliyunRAGStrategy> regAliyunRag("3");
static StrategyRegister<AliyunMcpStrategy> regAliyunMcp("4");
