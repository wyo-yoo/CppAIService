#pragma once                                        // 防止头文件被重复包含
#include <iostream>                                 // 引入标准输入输出流
#include <string>                                   // 引入 std::string 字符串类型
#include <curl/curl.h>                              // 引入 libcurl 库头文件（用于调用语音服务 HTTP API）
#include <fstream>                                  // 引入文件流（读取音频文件数据）
#include <memory>                                   // 引入智能指针
#include <sstream>                                  // 引入 std::stringstream 字符串流
#include <thread>                                   // 引入 std::thread 线程库
#include <chrono>                                   // 引入时间库
#include <mutex>                                    // 引入 std::mutex 互斥锁（保护 Token 缓存）


#include "../../../../HttpServer/include/utils/JsonUtil.h"   // 引入 JsonUtil 工具（json 类型定义）
#include"base64.h"                                  // 引入 base64 编解码工具（音频数据编码传输）



class AISpeechProcessor {                           // AI 语音处理器类：封装百度语音识别与语音合成能力
public:
    AISpeechProcessor(const std::string& clientId,          // 构造函数：传入百度语音服务的 API Key（client_id）
                      const std::string& clientSecret,      // 传入百度语音服务的 Secret Key（client_secret）
                      const std::string& cuid = "RZjSQGzNaA8EFWf6rvuHEKDh9i4XJIV9") //用户唯一标识，需要更改成自身标识（可选参数，带默认值）
        : client_id_(clientId), client_secret_(clientSecret), cuid_(cuid)   // 初始化列表：分别初始化三个成员变量
    {                                               // 构造函数体开始
        token_ = ensureValidToken();                // 取类级缓存的 Access Token（缓存有效则不发起网络请求）
    }                                               // 构造函数结束

    // 语音识别
    // speechData: 原始音频字节（PCM/WAV 等），内部自动做 base64 编码与长度计算
    std::string recognize(const std::string& speechData,const std::string& format = "pcm",int rate = 16000,int channel = 1);   // 语音识别：输入原始音频数据，返回识别出的文本（默认 PCM 格式、16k 采样率、单声道）

    // 语音合成
    // 返回值: 合成音频的下载地址 speech_url（需调用方再下载音频文件）
    std::string synthesize(const std::string& text,const std::string& format = "mp3-16k",const std::string& lang = "zh",int speed = 5,int pitch = 5,int volume = 5);   // 语音合成：输入文本，返回合成音频下载地址（默认 mp3-16k 格式、中文、语速/音调/音量均为 5）


private:
    std::string client_id_;                         // 百度语音 API Key（调用凭证）
    std::string client_secret_;                     // 百度语音 Secret Key（调用凭证）
    std::string cuid_;                              // 用户唯一标识（百度语音服务要求）
    std::string token_;                             // 缓存的 Access Token（通过 client_id/secret 换取）

    // 获取 Access Token
    std::string getAccessToken();                   // 调用百度鉴权接口获取 Access Token 并返回

    // ---- Token 类级缓存（所有实例共享，避免每个请求都去百度换 Token）----
    static inline std::string s_tokenCache_;                              // 缓存的 Access Token（所有实例共享）
    static inline std::mutex s_tokenMutex_;                               // 保护 Token 缓存的互斥锁（多线程并发刷新）
    static inline std::chrono::steady_clock::time_point s_tokenFetchedAt_; // Token 最近一次获取时间
    static constexpr int TOKEN_VALID_SECONDS = 29 * 24 * 3600;            // 百度 Token 有效期 30 天，提前 1 天刷新

    std::string ensureValidToken();                 // 返回当前有效 Token：缓存为空或临近过期时重新获取
    void forceRefreshToken();                       // 强制刷新 Token 缓存（收到 token 失效错误时调用）
};
