#pragma once
#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>
#include <memory>
#include <functional>
#include <chrono>

#include "utils/JsonUtil.h"



class AIStrategy {
public:
    virtual ~AIStrategy() = default;


    virtual std::string getApiUrl() const = 0;

    // API Key
    virtual std::string getApiKey() const = 0;


    virtual std::string getModel() const = 0;


    virtual json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const = 0;


    virtual std::string parseResponse(const json& response) const = 0;

    // 封装完整的对话流程，子类各自实现
    // messages: 对话历史，策略内部直接操作（push/pop）
    // userQuestion: 用户当前问题
    // httpExecutor: 执行 HTTP 请求的回调
    // 返回值: AI 的最终回答文本
    virtual std::string chat(
        std::vector<std::pair<std::string, long long>>& messages,
        const std::string& userQuestion,
        std::function<json(const json&, bool)> httpExecutor
    ) = 0;

protected:
    // 子类共用工具：获取当前时间戳（毫秒）
    static long long nowMs() {
        auto now = std::chrono::system_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }
};

class AliyunStrategy : public AIStrategy {

public:
    AliyunStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override;
    std::string parseResponse(const json& response) const override;

    std::string chat(
        std::vector<std::pair<std::string, long long>>& messages,
        const std::string& userQuestion,
        std::function<json(const json&, bool)> httpExecutor) override;

private:
    std::string apiKey_;
};

class DouBaoStrategy : public AIStrategy {

public:
    DouBaoStrategy() {
        const char* key = std::getenv("DOUBAO_API_KEY");
        if (!key) throw std::runtime_error("DOUBAO API Key not found!");
        apiKey_ = key;
    }
    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override;
    std::string parseResponse(const json& response) const override;

    std::string chat(
        std::vector<std::pair<std::string, long long>>& messages,
        const std::string& userQuestion,
        std::function<json(const json&, bool)> httpExecutor) override;

private:
    std::string apiKey_;
};

class AliyunRAGStrategy : public AIStrategy {

public:
    AliyunRAGStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override;
    std::string parseResponse(const json& response) const override;

    std::string chat(
        std::vector<std::pair<std::string, long long>>& messages,
        const std::string& userQuestion,
        std::function<json(const json&, bool)> httpExecutor) override;

private:
    std::string apiKey_;
};

class AliyunMcpStrategy : public AIStrategy {

public:
    AliyunMcpStrategy() {
        const char* key = std::getenv("DASHSCOPE_API_KEY");
        if (!key) throw std::runtime_error("Aliyun API Key not found!");
        apiKey_ = key;
    }

    std::string getApiUrl() const override;
    std::string getApiKey() const override;
    std::string getModel() const override;

    json buildRequest(const std::vector<std::pair<std::string, long long>>& messages) const override;
    std::string parseResponse(const json& response) const override;

    std::string chat(
        std::vector<std::pair<std::string, long long>>& messages,
        const std::string& userQuestion,
        std::function<json(const json&, bool)> httpExecutor) override;

private:
    std::string apiKey_;
};







