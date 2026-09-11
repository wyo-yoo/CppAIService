#include "ChatServer.h"
#include "security/Password.h"
#include <regex>

void ChatServer::handleAuthentication(const http::HttpRequest& req, http::HttpResponse* resp, bool registration) {
    try {
        auto body = json::parse(req.getBody());
        auto username = body.at("username").get<std::string>();
        auto password = body.at("password").get<std::string>();
        if (username.empty() || username.size() > 255 || password.empty() || password.size() > 1024)
            throw AccessError(400,"请输入有效的用户名和密码");
        if (registration && !std::regex_match(username,std::regex("[A-Za-z0-9_]{3,32}")))
            throw AccessError(400,"用户名请使用 3–32 位字母、数字或下划线");
        if (registration && !Password::validNew(password))
            throw AccessError(400,"密码请使用 15–128 个字符，可以使用容易记住的长句");
        if (!registration) access_->checkLogin(username);
        auto workerResp = *resp;
        // Expensive password work runs on a bounded pool, after the HTTP connection is registered.
        resp->setDeferredHandler([this,req,workerResp,username,password,registration]() mutable {
            bool accepted = authWorkers_.submit([this,req,workerResp,username,password,registration]() mutable {
                try {
                    auto result = mysqlUtil_.executeQuery("SELECT id,password FROM users WHERE username=?",username);
                    const bool exists = result->next();
                    const int uid = exists ? result->getInt("id") : -1;
                    const std::string encoded = exists ? std::string(result->getString("password")) : dummyPasswordHash_;
                    result.reset();
                    if (registration) {
                        if (exists) throw AccessError(409,"用户名已被注册，请换一个");
                        auto hash = Password::hash(password);
                        access_->reserveRegistration(access_->clientIp(req));
                        try { mysqlUtil_.executeUpdate("INSERT INTO users(username,password) VALUES (?,?)",username,hash); }
                        catch (...) {
                            auto duplicate = mysqlUtil_.executeQuery("SELECT id FROM users WHERE username=?",username);
                            if (duplicate->next()) throw AccessError(409,"用户名已被注册，请换一个");
                            throw;
                        }
                        auto created = mysqlUtil_.executeQuery("SELECT id FROM users WHERE username=?",username);
                        if (!created->next()) throw std::runtime_error("New account was not found");
                        const int newId = created->getInt("id"); created.reset();
                        accessReply(&workerResp,req,200,{{"success",true},{"status","success"},{"userId",newId},{"message","注册成功，请登录"}});
                    } else {
                        // Perform the same hash work for unknown names; don't expose account existence.
                        const bool matches = Password::verify(password,encoded);
                        if (!matches || !exists) throw AccessError(401,"用户名或密码错误");
                        auto session = getSessionManager()->createSession(req,&workerResp);
                        session->setValue("userId",std::to_string(uid));
                        session->setValue("username",username);
                        session->setValue("isLoggedIn","true");
                        accessReply(&workerResp,req,200,{{"success",true},{"userId",uid}});
                    }
                } catch (const AccessError& error) {
                    accessReply(&workerResp,req,error.status,{{"success",false},{"message",error.what()}},error.retryAfter);
                } catch (...) {
                    accessReply(&workerResp,req,503,{{"success",false},{"message","账号服务暂时不可用，请稍后重试"}},5);
                }
                sendDeferredResponse(workerResp);
            });
            if (!accepted) {
                accessReply(&workerResp,req,503,{{"success",false},{"message","登录或注册人数较多，请稍后重试"}},5);
                sendDeferredResponse(workerResp);
            }
        });
    } catch (const AccessError& error) {
        accessReply(resp,req,error.status,{{"success",false},{"message",error.what()}},error.retryAfter);
    } catch (...) { accessReply(resp,req,400,{{"success",false},{"message","请填写有效的账号信息"}}); }
}
