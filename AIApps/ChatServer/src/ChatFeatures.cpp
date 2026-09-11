#include "ChatServer.h"
#include "security/Password.h"
#include "AIUtil/AISessionIdGenerator.h"
#include <algorithm>
#include <cctype>
#include <regex>

namespace {
void reply(http::HttpResponse* response, const http::HttpRequest& request, int status, const json& body) {
    response->setStatusLine(request.getVersion(), static_cast<http::HttpResponse::HttpStatusCode>(status),
                           status == 200 ? "OK" : status == 401 ? "Unauthorized" : status == 404 ? "Not Found" : status == 409 ? "Conflict" : "Bad Request");
    auto text = body.dump(); response->setBody(text); response->setContentLength(text.size());
    response->setContentType("application/json; charset=utf-8"); response->setCloseConnection(false);
}
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
std::string titleFrom(const std::string& text) {
    std::string title = trim(text);
    size_t end = 0, count = 0;
    while (end < title.size() && count < 28) {
        unsigned char c = title[end];
        end += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++count;
    }
    title.resize(std::min(end, title.size()));
    for (char& c : title) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return title.empty() ? "新会话" : title;
}
bool validId(const std::string& id) {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); });
}
}

ChatServer::~ChatServer() {
    { std::lock_guard<std::mutex> lock(chatJobsMutex_); for (auto& item : chatJobs_) item.second->cancelled = true; }
    authWorkers_.shutdown();
    chatWorkers_.shutdown();
}
int ChatServer::authenticatedUser(const http::HttpRequest& request, http::HttpResponse* response, std::string* name) {
    auto session = getSessionManager()->getSession(request, response);
    if (session->getValue("isLoggedIn") != "true") {
        reply(response, request, 401, {{"success", false}, {"message", "请先登录"}}); return -1;
    }
    if (name) *name = session->getValue("username");
    return std::stoi(session->getValue("userId"));
}
void ChatServer::ensureSessionRecord(int userId, const std::string& id, const std::string& title) {
    mysqlUtil_.executeUpdate("INSERT IGNORE INTO chat_sessions (user_id,session_id,title,deleted) VALUES (?,?,?,0)", userId, id, title);
}
void ChatServer::initializeChatFeatures() {
    mysqlUtil_.executeUpdate("CREATE TABLE IF NOT EXISTS chat_sessions (user_id INT NOT NULL, session_id VARCHAR(64) NOT NULL, title VARCHAR(255) NOT NULL, deleted TINYINT NOT NULL DEFAULT 0, PRIMARY KEY(user_id,session_id)) CHARACTER SET utf8mb4");
    // 前端通过 POST 提交问题，并在同一次 HTTP 响应中持续读取 SSE 事件。
    httpServer_.Post("/chat/stream", [this](const auto& req, auto* resp) { handleChatStream(req, resp); });
    httpServer_.Post("/chat/cancel", [this](const auto& req, auto* resp) { handleChatCancel(req, resp); });
    httpServer_.Post("/chat/sessions/rename", [this](const auto& req, auto* resp) { handleSessionRename(req, resp); });
    httpServer_.Post("/chat/sessions/delete", [this](const auto& req, auto* resp) { handleSessionDelete(req, resp); });
}
// 流式业务入口：校验用户与会话 -> 注册响应流 -> 工作线程请求模型 -> 增量推送回答。
void ChatServer::handleChatStream(const http::HttpRequest& req, http::HttpResponse* resp, bool streaming, bool newSession) {
    try {
        std::string username;
        int userId = authenticatedUser(req, resp, &username); if (userId < 0) return;
        auto body = json::parse(req.getBody());
        auto question = trim(body.at("question").get<std::string>());
        auto model = body.value("modelType", std::string("5"));
        auto sid = newSession ? std::string() : body.value("sessionId", std::string());
        auto requestId = streaming ? body.at("requestId").get<std::string>() : Password::randomToken();
        if (question.empty() || question.size() > 8192) throw AccessError(400,"请输入问题，长度不超过 8KB");
        if (!access_->modelAllowed(model)) throw AccessError(400,"此模型暂未开放使用");
        if (!std::regex_match(requestId, std::regex("[A-Za-z0-9_-]{1,64}"))) throw AccessError(400,"无效的请求编号");
        if (!sid.empty() && !validId(sid)) throw AccessError(400,"无效的会话编号");
        std::unique_lock<std::mutex> jobsLock(chatJobsMutex_);
        if (chatJobs_.count(requestId)) throw AccessError(409,"请求编号已存在");
        std::shared_ptr<AIHelper> helper;
        std::string title;
        std::shared_ptr<PublicAccess::Permit> permit;
        {
            std::lock_guard<std::mutex> lock(mutexForChatInformation);
            auto& sessions = chatInformation[userId];
            if (!sid.empty() && !sessions.count(sid)) throw AccessError(404,"会话不存在或已删除");
            if (sid.empty() && sessions.size() >= 100) throw AccessError(409,"最多保留 100 个会话，请删除不再需要的会话");
            if (!sid.empty() && sessions.at(sid)->GetMessages().size() >= 200)
                throw AccessError(409,"此会话已达到 100 轮，请开始新会话");
            permit = access_->admit(userId,access_->clientIp(req));
            if (sid.empty()) {
                sid = AISessionIdGenerator().generate(); title = titleFrom(question);
                ensureSessionRecord(userId, sid, title);
                sessions[sid] = std::make_shared<AIHelper>(); sessionNames_[userId][sid] = title;
                std::lock_guard<std::mutex> idsLock(mutexForSessionsId);
                sessionsIdsMap[userId].push_back(sid);
            }
            helper = sessions.at(sid);
            title = sessionNames_[userId].count(sid) ? sessionNames_[userId].at(sid) : "会话 " + sid;
        }
        auto job = std::make_shared<ChatJob>(); job->userId = userId; job->sessionId = sid; job->requestId = requestId;
        job->permit = std::move(permit); chatJobs_[requestId] = job;
        auto dispatch = [this,job,helper,username,question,model,title,req](std::shared_ptr<http::ResponseStream> stream, http::HttpResponse response) {
            if (stream) stream->send("meta", json{{"requestId",job->requestId},{"sessionId",job->sessionId},{"name",title}}.dump());
            auto release = [this,job] {
                std::lock_guard<std::mutex> lock(chatJobsMutex_); chatJobs_.erase(job->requestId); job->permit.reset();
            };
            bool accepted = chatWorkers_.submit([this,job,helper,username,question,model,stream,response,req,release]() mutable {
                try {
                    if (stream) stream->send("status",json{{"message","正在等待模型…"}}.dump());
                    ChatTransport::Delta delta;
                    if (stream) delta = [stream](const std::string& text) {
                        if (!stream->send("delta",json{{"text",text}}.dump())) throw ChatCancelled();
                    };
                    auto result = helper->chatStream(job->userId,username,job->sessionId,question,model,delta,
                        [job,stream]{ return job->cancelled.load() || (stream && stream->closed()); });
                    release();
                    if (stream) stream->send("done",json{{"sessionId",job->sessionId},{"stopped",result.stopped},{"text",result.text}}.dump());
                    else accessReply(&response,req,200,{{"success",true},{"sessionId",job->sessionId},{"Information",result.text}});
                } catch (...) {
                    release();
                    if (stream) stream->send("error",json{{"message","生成失败，请稍后重试"}}.dump());
                    else accessReply(&response,req,503,{{"success",false},{"message","生成失败，请稍后重试"}});
                }
                if (stream) stream->finish(); else sendDeferredResponse(response);
            });
            if (!accepted) {
                release();
                if (stream) { stream->send("error",json{{"message","服务繁忙，请稍后重试"}}.dump()); stream->finish(); }
                else { accessReply(&response,req,503,{{"message","服务繁忙，请稍后重试"}},5); sendDeferredResponse(response); }
            }
        };
        if (streaming) {
            resp->setStatusLine(req.getVersion(),http::HttpResponse::k200Ok,"OK");
            resp->setStreamHandler([dispatch](std::shared_ptr<http::ResponseStream> stream) { dispatch(stream,http::HttpResponse()); });
        } else {
            auto response = *resp;
            resp->setDeferredHandler([dispatch,response] { dispatch(nullptr,response); });
        }
    } catch (const AccessError& error) {
        accessReply(resp,req,error.status,{{"success",false},{"message",error.what()}},error.retryAfter);
    } catch (...) { accessReply(resp,req,400,{{"success",false},{"message","请求无效或会话暂时无法保存"}}); }
}
void ChatServer::handleChatCancel(const http::HttpRequest& req, http::HttpResponse* resp) {
    try {
        int uid = authenticatedUser(req, resp); if (uid < 0) return;
        auto id = json::parse(req.getBody()).at("requestId").get<std::string>();
        std::lock_guard<std::mutex> lock(chatJobsMutex_);
        auto it = chatJobs_.find(id);
        if (it == chatJobs_.end() || it->second->userId != uid) {
            reply(resp, req, 404, {{"success", false}, {"message", "请求已结束或不存在"}}); return;
        }
        // 取消接口只设置标记，由模型传输循环检查并结束生成，保留已收到的回答片段。
        it->second->cancelled = true;
        reply(resp, req, 200, {{"success", true}});
    } catch (...) { reply(resp, req, 400, {{"success", false}, {"message", "无效的取消请求"}}); }
}
void ChatServer::handleSessionRename(const http::HttpRequest& req, http::HttpResponse* resp) {
    try {
        int uid = authenticatedUser(req, resp); if (uid < 0) return;
        auto body = json::parse(req.getBody());
        auto sid = body.at("sessionId").get<std::string>(); auto title = trim(body.at("name").get<std::string>());
        if (!validId(sid) || title.empty() || title.size() > 240) throw std::runtime_error("Invalid session title");
        std::lock_guard<std::mutex> lock(mutexForChatInformation);
        if (!chatInformation[uid].count(sid)) { reply(resp, req, 404, {{"success", false}, {"message", "会话不存在"}}); return; }
        ensureSessionRecord(uid, sid, title);
        mysqlUtil_.executeUpdate("UPDATE chat_sessions SET title=? WHERE user_id=? AND session_id=? AND deleted=0", title, uid, sid);
        sessionNames_[uid][sid] = title;
        reply(resp, req, 200, {{"success", true}, {"sessionId", sid}, {"name", title}});
    } catch (...) { reply(resp, req, 400, {{"success", false}, {"message", "重命名失败，请输入有效名称"}}); }
}
void ChatServer::handleSessionDelete(const http::HttpRequest& req, http::HttpResponse* resp) {
    try {
        int uid = authenticatedUser(req, resp); if (uid < 0) return;
        auto sid = json::parse(req.getBody()).at("sessionId").get<std::string>();
        if (!validId(sid)) throw std::runtime_error("Invalid session");
        std::lock_guard<std::mutex> jobsLock(chatJobsMutex_);
        for (const auto& item : chatJobs_) if (item.second->userId == uid && item.second->sessionId == sid) {
            reply(resp, req, 409, {{"success", false}, {"message", "请先停止当前会话的生成，再删除"}}); return;
        }
        std::lock_guard<std::mutex> lock(mutexForChatInformation);
        if (!chatInformation[uid].count(sid)) { reply(resp, req, 404, {{"success", false}, {"message", "会话不存在"}}); return; }
        ensureSessionRecord(uid, sid, "已删除会话");
        // The tombstone also prevents delayed MQ deliveries restoring messages.
        mysqlUtil_.executeUpdate("UPDATE chat_sessions SET deleted=1 WHERE user_id=? AND session_id=?", uid, sid);
        // Logical deletion is the durable operation; retain existing message rows
        // for now so a cleanup failure cannot leave database and memory disagreeing.
        chatInformation[uid].erase(sid); sessionNames_[uid].erase(sid);
        { std::lock_guard<std::mutex> idsLock(mutexForSessionsId);
          auto& ids = sessionsIdsMap[uid]; ids.erase(std::remove(ids.begin(), ids.end(), sid), ids.end()); }
        reply(resp, req, 200, {{"success", true}});
    } catch (...) { reply(resp, req, 400, {{"success", false}, {"message", "删除失败，请稍后重试"}}); }
}
