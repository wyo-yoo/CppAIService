#include "handlers/ChatSpeechHandler.h"
void ChatSpeechHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) {
    try {
        int uid = server_->authenticatedUser(req,resp); if (uid < 0) return;
        if (!server_->access_->speechEnabled()) throw AccessError(403,"语音服务暂未开放");
        auto text = json::parse(req.getBody()).at("text").get<std::string>();
        if (text.empty() || text.size() > 8192) throw AccessError(400,"朗读内容应为 1–8192 字节");
        const char* secret = std::getenv("BAIDU_CLIENT_SECRET");
        const char* id = std::getenv("BAIDU_CLIENT_ID");
        if (!secret || !*secret || !id || !*id) throw AccessError(503,"语音服务暂时不可用");
        std::string clientId(id), clientSecret(secret);
        auto permit = server_->access_->admit(uid,server_->access_->clientIp(req));
        auto response = *resp;
        resp->setDeferredHandler([this,req,text,clientId,clientSecret,permit,response]() mutable {
            bool accepted = server_->chatWorkers_.submit([this,req,text,clientId,clientSecret,permit,response]() mutable {
                try {
                    AISpeechProcessor speech(clientId,clientSecret);
                    auto url = speech.synthesize(text,"mp3-16k","zh",5,5,5);
                    permit.reset();
                    accessReply(&response,req,200,{{"success",true},{"url",url}});
                } catch (...) { permit.reset(); accessReply(&response,req,503,{{"message","语音生成失败，请稍后重试"}}); }
                server_->sendDeferredResponse(response);
            });
            if (!accepted) { accessReply(&response,req,503,{{"message","服务繁忙，请稍后重试"}},5); server_->sendDeferredResponse(response); }
        });
    } catch (const AccessError& error) {
        accessReply(resp,req,error.status,{{"success",false},{"message",error.what()}},error.retryAfter);
    } catch (...) { accessReply(resp,req,400,{{"success",false},{"message","请输入有效的朗读内容"}}); }
}
