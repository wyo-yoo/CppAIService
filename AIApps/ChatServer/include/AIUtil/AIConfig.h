#pragma once                                        // 防止头文件被重复包含
#include <string>                                   // 引入 std::string 字符串类型
#include <unordered_map>                            // 引入 std::unordered_map 哈希表
#include <vector>                                   // 引入 std::vector 动态数组
#include <regex>                                    // 引入 std::regex 正则表达式（用于解析 AI 响应）
#include <fstream>                                  // 引入 std::ifstream 文件读取流（用于加载配置文件）
#include <sstream>                                  // 引入 std::stringstream 字符串流
#include <iostream>                                 // 引入标准输入输出流
#include "../../../../HttpServer/include/utils/JsonUtil.h"     // 引入 JsonUtil 工具（json 类型定义）


struct AITool {                                     // AI 工具描述结构体：定义可供 AI 调用的外部工具信息
    std::string name;                               // 工具名称
    std::unordered_map<std::string, std::string> params;   // 工具参数表：参数名 -> 参数说明
    std::string desc;                               // 工具功能描述
};


struct AIToolCall {                                 // AI 工具调用结构体：表示 AI 响应中解析出的工具调用请求
    std::string toolName;                           // 被调用的工具名称
    json args;                                      // 工具调用参数（JSON 格式）
    bool isToolCall = false;                        // 是否为工具调用（默认 false 表示普通回答）
};


class AIConfig {                                    // AI 配置类：负责加载配置、构建提示词、解析 AI 响应
public:
    bool loadFromFile(const std::string& path);     // 从指定路径加载配置文件，成功返回 true
    std::string buildPrompt(const std::string& userInput) const;   // 根据用户输入构建发送给 AI 的完整提示词
    AIToolCall parseAIResponse(const std::string& response) const; // 解析 AI 响应文本，提取工具调用信息
    std::string buildToolResultPrompt(const std::string& userInput,const std::string& toolName,const json& toolArgs,const json& toolResult) const;   // 构建工具执行结果提示词：用户输入 + 工具名 + 工具参数 + 工具结果

private:
    std::string promptTemplate_;                    // 提示词模板（从配置文件中加载）
    std::vector<AITool> tools_;                     // 可用工具列表（从配置文件中加载）

    std::string buildToolList() const;              // 将工具列表格式化为文本，用于拼接到提示词中
};
