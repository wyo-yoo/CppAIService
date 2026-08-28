#pragma once                                        // 防止头文件被重复包含
#include <string>                                   // 引入 std::string 字符串类型
#include <vector>                                   // 引入 std::vector 动态数组
#include <utility>                                  // 引入 std::pair 等工具组件
#include <curl/curl.h>                              // 引入 libcurl 库头文件（用于发起 HTTP 请求）
#include <iostream>                                 // 引入标准输入输出流
#include <sstream>                                  // 引入 std::stringstream 字符串流

#include "../../../../HttpServer/include/utils/JsonUtil.h"           // 引入 JsonUtil 工具（json 类型定义）
#include"../../../../HttpServer/include/utils/MysqlUtil.h"           // 引入 MysqlUtil 工具（MySQL 数据库操作）

#include"AIFactory.h"                               // 引入 AI 策略工厂（策略模式相关定义）


//这边封装curl去访问对阿里的模型
class AIHelper {                                    // AI 助手封装类：管理对话历史、策略调用与持久化
public:
    // 构造函数，初始化API Key
    AIHelper();                                     // 构造函数声明：初始化 API Key 等成员

    // 设置默认模型
    //void setModel(const std::string& modelName);  // 已废弃的接口：曾用于设置默认模型

    void setStrategy(std::shared_ptr<AIStrategy> strat);   // 设置当前使用的 AI 策略对象（策略模式注入）

    // 添加一条消息
    void addMessage(int userId, const std::string& userName, bool is_user, const std::string& userInput, std::string sessionId);   // 添加一条消息到历史记录并异步落库
    // 恢复一条消息
    void restoreMessage(const std::string& userInput, long long ms);   // 从数据库恢复一条历史消息到内存

    // 发送聊天消息，返回AI的响应内容
    // 流程编排已下沉到 AIStrategy::chat()，此处仅委托 + 持久化到 MySQL
    std::string chat(int userId, std::string userName, std::string sessionId, std::string userQuestion, std::string modelType);   // 聊天主入口：委托策略处理并持久化结果

    // 可选：发送自定义请求体
    json request(const json& payload);              // 发送自定义 JSON 请求体，返回响应 JSON

    std::vector<std::pair<std::string, long long>> GetMessages();   // 获取当前用户的完整历史对话列表

private:
    std::string escapeString(const std::string& input);   // 字符串转义：防止 SQL 注入（落库前处理）
    //加入到mysql的接口（提供加入到线程池的接口，线程池做异步mysql更新操作）
    //todo:
    void pushMessageToMysql(int userId, const std::string& userName, bool is_user, const std::string& userInput, long long ms,std::string sessionId);   // 将消息推入线程池，异步写入 MySQL

    // 内部方法：执行curl请求，返回原始JSON
    json executeCurl(const json& payload);          // 执行 curl HTTP 请求并返回原始响应 JSON
    // curl 回调函数，把返回的数据写到 string buffer
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);   // curl 写回调：把响应数据追加到 string buffer

private:

    /*
    * 重构代码，将其使用策略模式&&工厂模式抽离出来
    std::string apiKey_;
    //默认用通义千问
    std::string model_ = "qwen-plus";
    //对应地址
    std::string apiUrl_ = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    */
    std::shared_ptr<AIStrategy> strategy;           // 当前使用的 AI 策略对象（重构后替代 apiKey_/model_/apiUrl_）

    //一个用户针对一个AIHelper，messages存放用户的历史对话
    //偶数下标代表用户的信息，奇数下标是ai返回的内容
    //后者代表时间戳
    std::vector<std::pair<std::string, long long>> messages;   // 历史对话列表：<内容, 时间戳>，偶数下标为用户消息、奇数下标为 AI 回复

    //http::MysqlUtil mysqlUtil_;                   // 已注释：原先直接持有 MySQL 工具（现改为线程池异步落库）
};
