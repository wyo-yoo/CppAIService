#include "../include/handlers/ChatSpeechHandler.h"
#include <thread>


void ChatSpeechHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        // ---- 会话鉴权（快速操作，留在网络线程执行）----
        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        // ---- 解析请求参数 ----
        std::string text;

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("text")) text = j["text"];
        }


        const char* secretEnv = std::getenv("BAIDU_CLIENT_SECRET");
        const char* idEnv = std::getenv("BAIDU_CLIENT_ID");

        if (!secretEnv) throw std::runtime_error("BAIDU_CLIENT_SECRET not found!");
        if (!idEnv) throw std::runtime_error("BAIDU_CLIENT_ID not found!");

        std::string clientSecret(secretEnv);
        std::string clientId(idEnv);

        // ---- 标记延迟响应：网络线程立即返回，语音合成放到工作线程执行 ----
        // 语音合成最长阻塞 60 秒（创建任务 + 轮询），不能阻塞 muduo 网络线程
        resp->setDeferred(true);
        http::HttpResponse workerResp = *resp;   // 拷贝响应对象（携带 HttpServer 分配的延迟 ID）
        std::string version = req.getVersion();  // 拷贝 HTTP 版本号供工作线程使用

        std::thread worker([this, version, text, clientId, clientSecret, workerResp]() mutable {
            json resultResp;
            try
            {
                // 耗时的百度语音合成调用（Token 已做类级缓存，跨请求复用）
                AISpeechProcessor speechProcessor(clientId, clientSecret);

                std::string speechUrl = speechProcessor.synthesize(text,
                                                                   "mp3-16k",
                                                                   "zh",
                                                                    5,
                                                                    5,
                                                                    5 );

                resultResp["success"] = true;
                resultResp["url"] = speechUrl;
                std::string successBody = resultResp.dump(4);
                server_->packageResp(version, http::HttpResponse::k200Ok, "OK", false,
                    "application/json", successBody.size(), successBody, &workerResp);
            }
            catch (const std::exception& e)
            {
                // 工作线程内的失败也通过延迟响应返回给客户端
                resultResp["status"] = "error";
                resultResp["message"] = e.what();
                std::string failureBody = resultResp.dump(4);
                server_->packageResp(version, http::HttpResponse::k400BadRequest, "Bad Request", true,
                    "application/json", failureBody.size(), failureBody, &workerResp);
            }
            // 工作线程完成后发送延迟响应（内部通过延迟 ID 找回连接，线程安全）
            server_->sendDeferredResponse(workerResp);
        });
        worker.detach();
        return;
    }
    catch (const std::exception& e)
    {
        // 同步错误路径（参数解析、环境变量缺失等）保持不变
        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}
