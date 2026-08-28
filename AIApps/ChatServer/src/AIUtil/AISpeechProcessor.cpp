 #include"../include/AIUtil/AISpeechProcessor.h"

// #include"AISpeechProcessor.h"

#include <muduo/base/Logging.h>


// libcurl 写回调：把响应数据追加到 string buffer
static size_t onWriteData(void* buffer, size_t size, size_t nmemb, void* userp) {
    std::string* str = static_cast<std::string*>(userp);
    str->append((char*)buffer, size * nmemb);
    return size * nmemb;
}

// 为 curl 设置通用选项：超时时间（避免网络异常时无限阻塞调用线程）
static void setCommonCurlOpts(CURL* curl, long timeoutSec) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);   // 建连超时 5 秒
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);  // 整个请求超时（秒）
}

// 判断百度返回的错误码是否为 token 失效（110/111/112）
// 110: Access Token 无效；111: Access Token 过期；112: Token 未授权当前接口
static bool isTokenError(long errNo) {
    return errNo == 110 || errNo == 111 || errNo == 112;
}

// 从百度返回的 JSON 中提取错误码（识别接口为 err_no，其他接口为 error_code）
static bool extractErrNo(const json& j, long& outErrNo) {
    if (j.contains("err_no") && j["err_no"].is_number()) {
        outErrNo = j["err_no"].get<long>();
        return true;
    }
    if (j.contains("error_code") && j["error_code"].is_number()) {
        outErrNo = j["error_code"].get<long>();
        return true;
    }
    return false;
}

std::string AISpeechProcessor::getAccessToken() {
    std::string result;
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR << "AISpeechProcessor: curl_easy_init failed in getAccessToken";
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, "https://aip.baidubce.com/oauth/2.0/token");
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");
    setCommonCurlOpts(curl, 10L);   // 鉴权请求超时 10 秒

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::string data = "grant_type=client_credentials&client_id=" + client_id_ + "&client_secret=" + client_secret_;
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

    CURLcode res = curl_easy_perform(curl);

    curl_easy_cleanup(curl);
    if (headers) curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        LOG_ERROR << "AISpeechProcessor: getAccessToken curl error: " << curl_easy_strerror(res);
        return "";
    }

    // 解析为 nlohmann::json
    try {
        auto j = json::parse(result);
        if (j.contains("access_token") && j["access_token"].is_string()) {
            return j["access_token"].get<std::string>();
        }
        long errNo = 0;
        if (extractErrNo(j, errNo)) {
            LOG_ERROR << "AISpeechProcessor: getAccessToken failed, error_code=" << errNo
                      << ", response=" << result;
        }
    } catch (...) {
        LOG_ERROR << "AISpeechProcessor: getAccessToken parse error: " << result;
    }
    return "";
}

// 返回当前有效 Token：缓存为空或临近过期（30 天）时重新向百度获取
std::string AISpeechProcessor::ensureValidToken() {
    std::lock_guard<std::mutex> lock(s_tokenMutex_);   // 加锁避免多线程同时刷新

    auto now = std::chrono::steady_clock::now();
    bool expired = (s_tokenFetchedAt_.time_since_epoch().count() == 0) ||
                   (std::chrono::duration_cast<std::chrono::seconds>(now - s_tokenFetchedAt_).count()
                    >= TOKEN_VALID_SECONDS);

    if (s_tokenCache_.empty() || expired) {
        LOG_INFO << "AISpeechProcessor: refreshing access token...";
        std::string newToken = getAccessToken();
        if (!newToken.empty()) {
            s_tokenCache_ = newToken;
            s_tokenFetchedAt_ = now;
        } else {
            LOG_ERROR << "AISpeechProcessor: refresh access token failed";
        }
    }
    return s_tokenCache_;
}

// 强制刷新 Token 缓存（收到百度 token 失效错误码后调用）
void AISpeechProcessor::forceRefreshToken() {
    std::lock_guard<std::mutex> lock(s_tokenMutex_);
    LOG_INFO << "AISpeechProcessor: force refresh token (invalid token error)";
    std::string newToken = getAccessToken();
    if (!newToken.empty()) {
        s_tokenCache_ = newToken;
        s_tokenFetchedAt_ = std::chrono::steady_clock::now();
    }
}


// 语音识别
std::string AISpeechProcessor::recognize(const std::string& speechData,
                                         const std::string& format,
                                         int rate,
                                         int channel)
{
    // 最多尝试 2 次：首次失败且是 token 失效时，刷新 token 后重试一次
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string token = ensureValidToken();
        if (token.empty()) {
            LOG_ERROR << "AISpeechProcessor: no valid token for recognize";
            return "";
        }

        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string result;

        curl_easy_setopt(curl, CURLOPT_URL, "https://vop.baidu.com/server_api");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");
        setCommonCurlOpts(curl, 60L);   // 识别最长音频约 60 秒

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // 构造 JSON：speech 字段必须是 base64 编码后的音频，len 是编码前原始字节长度
        json body;
        body["format"] = format;
        body["rate"] = rate;
        body["channel"] = channel;
        body["cuid"] = cuid_;
        body["token"] = token;
        body["len"] = static_cast<int>(speechData.size());            // 原始音频字节长度（base64 之前）
        body["speech"] = base64_encode(speechData);                   // 内部完成 base64 编码

        std::string data = body.dump();   // 注意：dump 只转义引号与反斜杠，base64 的 + / = 不会被改动

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

        CURLcode res = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        if (headers) curl_slist_free_all(headers);
        if (res != CURLE_OK) {
            LOG_ERROR << "AISpeechProcessor: recognize curl error: " << curl_easy_strerror(res);
            return "";
        }

        // 解析返回 JSON
        try {
            json root = json::parse(result);
            long errNo = 0;
            if (extractErrNo(root, errNo) && isTokenError(errNo) && attempt == 0) {
                // token 失效：强制刷新后重试一次
                LOG_WARN << "AISpeechProcessor: token invalid (err_no=" << errNo << "), retry once";
                forceRefreshToken();
                continue;
            }
            if (root.contains("result") && root["result"].is_array() && !root["result"].empty()) {
                if (root["result"][0].is_string()) {
                    return root["result"][0].get<std::string>();
                }
            }
            if (extractErrNo(root, errNo)) {
                LOG_ERROR << "AISpeechProcessor: recognize failed, err_no=" << errNo
                          << ", response=" << result;
            }
        } catch (...) {
            LOG_ERROR << "AISpeechProcessor: parse error in recognize response: " << result;
        }

        LOG_ERROR << "AISpeechProcessor: recognize failed, response: " << result;
        return "";
    }
    return "";
}



// 语音合成（创建任务 -> 轮询 -> 返回 speech_url）
std::string AISpeechProcessor::synthesize(const std::string& text,
                                          const std::string& format,
                                          const std::string& lang,
                                          int speed,
                                          int pitch,
                                          int volume)
{
    // 最多尝试 2 次创建任务：token 失效时刷新后重试一次
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string token = ensureValidToken();
        if (token.empty()) {
            LOG_ERROR << "AISpeechProcessor: no valid token for synthesize";
            return "";
        }

        CURL* curl = nullptr;
        CURLcode res;
        std::string response;

        // ----------- 第一步：创建合成任务 -----------
        curl = curl_easy_init();
        if (!curl) return "";

        std::string create_url = "https://aip.baidubce.com/rpc/2.0/tts/v1/create?access_token=" + token;

        curl_easy_setopt(curl, CURLOPT_URL, create_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // 按你示例使用 undefined（和官方 demo 一致）
        curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");
        setCommonCurlOpts(curl, 10L);   // 创建任务超时 10 秒

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        json body = {
            {"text", text},
            {"format", format},
            {"lang", lang},
            {"speed", speed},
            {"pitch", pitch},
            {"volume", volume},
            {"enable_subtitle", 0}
        };

        std::string data = body.dump();

        response.clear();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            if (headers) curl_slist_free_all(headers);
            LOG_ERROR << "AISpeechProcessor: synthesize create curl error: " << curl_easy_strerror(res);
            return "";
        }

        curl_easy_cleanup(curl);
        if (headers) curl_slist_free_all(headers);

        // 解析 task_id
        std::string task_id;
        try {
            json result_json = json::parse(response);
            // token 失效：刷新后重试一次
            long errNo = 0;
            if (extractErrNo(result_json, errNo) && isTokenError(errNo) && attempt == 0) {
                LOG_WARN << "AISpeechProcessor: token invalid (error_code=" << errNo << "), retry once";
                forceRefreshToken();
                continue;
            }
            // 有些示例中 task_id 在根节点或者在 tasks_info[0] 中都可能出现，优先取根 task_id，再尝试 tasks_info
            if (result_json.contains("task_id") && result_json["task_id"].is_string()) {
                task_id = result_json["task_id"].get<std::string>();
            } else if (result_json.contains("tasks_info") && result_json["tasks_info"].is_array()
                       && !result_json["tasks_info"].empty() && result_json["tasks_info"][0].contains("task_id")) {
                task_id = result_json["tasks_info"][0]["task_id"].get<std::string>();
            }
            if (task_id.empty()) {
                if (extractErrNo(result_json, errNo)) {
                    LOG_ERROR << "AISpeechProcessor: create task failed, error_code=" << errNo
                              << ", response=" << response;
                }
            }
        } catch (...) {
            LOG_ERROR << "AISpeechProcessor: parse error in create response: " << response;
            return "";
        }

        if (task_id.empty()) return "";

        // ----------- 第二步：轮询查询任务状态 -----------
        std::string speech_url;
        json query;
        query["task_ids"] = json::array({task_id});

        // 轮询，上限可按需调整（例如超时 30 次）
        const int max_loops = 60; // 最多等 60 秒（sleep 1s）
        int loops = 0;
        while (loops++ < max_loops) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            curl = curl_easy_init();
            if (!curl) break;

            std::string query_url = "https://aip.baidubce.com/rpc/2.0/tts/v1/query?access_token=" + token;
            curl_easy_setopt(curl, CURLOPT_URL, query_url.c_str());
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "undefined");
            setCommonCurlOpts(curl, 10L);   // 查询任务状态超时 10 秒

            headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            data = query.dump();
            response.clear();
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onWriteData);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

            res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            if (headers) curl_slist_free_all(headers);
            if (res != CURLE_OK) {
                LOG_ERROR << "AISpeechProcessor: synthesize query curl error: " << curl_easy_strerror(res);
                break;
            }

            // 解析轮询结果
            try {
                json queryResult = json::parse(response);
                if (queryResult.contains("tasks_info") && queryResult["tasks_info"].is_array()
                    && !queryResult["tasks_info"].empty()) {
                    json task = queryResult["tasks_info"][0];
                    if (task.contains("task_status") && task["task_status"].is_string()) {
                        std::string status = task["task_status"].get<std::string>();
                        if (status == "Success" && task.contains("task_result") && task["task_result"].contains("speech_url")) {
                            speech_url = task["task_result"]["speech_url"].get<std::string>();
                            break;
                        }
                        if (status == "Failure") {
                            LOG_ERROR << "AISpeechProcessor: synthesize task failed, response=" << response;
                            return "";
                        }
                    }
                }
            } catch (...) {
                LOG_ERROR << "AISpeechProcessor: parse error in query response: " << response;
                break;
            }
        }

        if (speech_url.empty()) {
            LOG_ERROR << "AISpeechProcessor: synthesize timeout after " << max_loops << "s polling";
        }
        return speech_url;
    }
    return "";
}
