#include "security/PublicAccess.h"
#include "security/Password.h"
#include <arpa/inet.h>
#include <algorithm>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace {
std::string env(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name); return value ? value : fallback;
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); }); return text;
}
}
int settingInt(const char* name, int fallback, int minimum, int maximum) {
    auto text = env(name, std::to_string(fallback).c_str());
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error(std::string("Invalid setting: ") + name);
    long value = std::stol(text);
    if (value < minimum || value > maximum) throw std::runtime_error(std::string("Out-of-range setting: ") + name);
    return static_cast<int>(value);
}
void accessReply(http::HttpResponse* response, const http::HttpRequest& request, int status, const json& body, int retry) {
    const std::map<int, std::string> reasons{{200,"OK"},{400,"Bad Request"},{401,"Unauthorized"},
        {403,"Forbidden"},{404,"Not Found"},{409,"Conflict"},{413,"Content Too Large"},
        {415,"Unsupported Media Type"},{429,"Too Many Requests"},{500,"Internal Server Error"},{503,"Service Unavailable"}};
    response->setStatusLine(request.getVersion(), static_cast<http::HttpResponse::HttpStatusCode>(status), reasons.at(status));
    auto text = body.dump(); response->setBody(text); response->setContentLength(text.size());
    response->setContentType("application/json; charset=utf-8"); response->setCloseConnection(false);
    response->addHeader("Cache-Control", "no-store");
    if (retry) response->addHeader("Retry-After", std::to_string(retry));
}
PublicAccess::PublicAccess()
    : origin_(env("CHAT_PUBLIC_ORIGIN")), trustProxy_(settingInt("CHAT_TRUST_PROXY",0,0,1)),
      registration_(settingInt("CHAT_REGISTRATION_OPEN",1,0,1)), speech_(settingInt("CHAT_ENABLE_TTS",0,0,1)),
      userDaily_(settingInt("CHAT_USER_DAILY_LIMIT",20,1,10000)), siteDaily_(settingInt("CHAT_SITE_DAILY_LIMIT",200,1,100000)),
      concurrent_(settingInt("CHAT_MAX_CONCURRENT",4,1,4)), perMinute_(settingInt("CHAT_USER_PER_MINUTE",6,1,120)),
      registrationsDaily_(settingInt("CHAT_REGISTRATIONS_DAILY",50,1,10000)),
      registrationsIpDaily_(settingInt("CHAT_REGISTRATIONS_IP_DAILY",5,1,1000)) {
    if (!origin_.empty() && !std::regex_match(origin_, std::regex("https://[A-Za-z0-9.-]+(:[0-9]{1,5})?")))
        throw std::runtime_error("CHAT_PUBLIC_ORIGIN must be an HTTPS origin without a trailing slash");
    if (trustProxy_ && (origin_.empty() || env("CHAT_BIND_ADDRESS", "127.0.0.1") != "127.0.0.1"))
        throw std::runtime_error("Proxy mode requires CHAT_PUBLIC_ORIGIN and a loopback-only listener");
    std::stringstream source(env("CHAT_ALLOWED_MODELS", "5")); std::string id;
    while (std::getline(source, id, ',')) {
        if (id.size() != 1 || id[0] < '1' || id[0] > '5') throw std::runtime_error("Invalid CHAT_ALLOWED_MODELS");
        models_.insert(id);
    }
    if (models_.empty()) throw std::runtime_error("Enable at least one model");
}
void PublicAccess::initializeDatabase() {
    mysql_.executeUpdate("CREATE TABLE IF NOT EXISTS usage_daily (day DATE NOT NULL, scope VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL, used INT UNSIGNED NOT NULL DEFAULT 0, PRIMARY KEY(day,scope)) ENGINE=InnoDB");
    // Upgrade existing accounts before listening. Plaintext authentication is never enabled.
    for (;;) {
        std::vector<std::pair<int,std::string>> rows;
        auto result = mysql_.executeQuery("SELECT id,password FROM users WHERE BINARY LEFT(password,14) <> 'pbkdf2_sha256$' LIMIT 100");
        while (result->next()) rows.emplace_back(result->getInt("id"), result->getString("password"));
        result.reset();
        if (rows.empty()) break;
        for (const auto& row : rows)
            mysql_.executeUpdate("UPDATE users SET password=? WHERE id=? AND BINARY password=?", Password::hash(row.second), row.first, row.second);
    }
    mysql_.executeUpdate("DELETE FROM usage_daily WHERE day < UTC_DATE() - INTERVAL 90 DAY");
}
std::string PublicAccess::clientIp(const http::HttpRequest& request) const {
    auto peer = request.peerIp();
    if (trustProxy_ && peer == "127.0.0.1") {
        // Nginx must overwrite X-Real-IP. Never trust arbitrary X-Forwarded-For.
        auto value = request.getHeader("X-Real-IP");
        unsigned char address[16];
        if (inet_pton(AF_INET, value.c_str(), address) == 1 || inet_pton(AF_INET6, value.c_str(), address) == 1) return value;
        throw AccessError(400, "代理未提供有效的客户端地址");
    }
    return peer.empty() ? "unknown" : peer;
}
void PublicAccess::rate(const std::string& key, int limit, int seconds) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(ratesMutex_);
    if (windows_.size() >= 10000) {
        for (auto it = windows_.begin(); it != windows_.end();)
            if (it->second.expires <= now) it = windows_.erase(it); else ++it;
        if (windows_.size() >= 10000 && !windows_.count(key)) throw AccessError(503,"服务繁忙，请稍后重试",60);
    }
    auto& window = windows_[key];
    if (window.expires <= now) window = Window{0,now + std::chrono::seconds(seconds)};
    if (window.count >= limit) throw AccessError(429,"操作太频繁，请稍后重试",std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(window.expires-now).count())+1));
    ++window.count;
}
void PublicAccess::before(http::HttpRequest& request) {
    try {
        const auto ip = clientIp(request);
        rate("request:"+ip,240,60);
        if (request.method() == http::HttpRequest::kPost) {
            auto contentType = lower(request.getHeader("Content-Type"));
            auto end = contentType.find(';'); contentType = contentType.substr(0,end);
            while (!contentType.empty() && contentType.back() == ' ') contentType.pop_back();
            if (contentType != "application/json") throw AccessError(415,"请使用 JSON 提交请求");
            if (request.getBody().size() > 65536) throw AccessError(413,"提交的内容太长");
            auto origin = request.getHeader("Origin");
            const auto expected = origin_.empty() ? "http://" + request.getHeader("Host") : origin_;
            if ((!origin.empty() && origin != expected) || request.getHeader("Sec-Fetch-Site") == "cross-site")
                throw AccessError(403,"请从本站页面提交请求");
        }
        if (request.path() == "/login" || request.path() == "/register") {
            if (request.getBody().size() > 4096) throw AccessError(413,"账号信息太长");
            rate("auth-ip:"+ip,30,900);
            rate("auth-site",120,60);
        }
        if (request.path() == "/register") {
            if (!registration_) throw AccessError(403,"网站暂时关闭新用户注册");
            rate("register:"+ip,10,3600);
        }
    } catch (const AccessError& error) {
        http::HttpResponse response;
        accessReply(&response,request,error.status,{{"success",false},{"message",error.what()}},error.retryAfter);
        after(response); throw response;
    }
}
void PublicAccess::after(http::HttpResponse& response) {
    response.addHeader("X-Content-Type-Options","nosniff");
    response.addHeader("X-Frame-Options","DENY");
    response.addHeader("Referrer-Policy","same-origin");
    response.addHeader("Cache-Control","no-store");
    if (!origin_.empty()) response.addHeader("Strict-Transport-Security","max-age=31536000");
}
void PublicAccess::checkLogin(const std::string& username) { rate("login-name:"+lower(username),10,900); }
void PublicAccess::reserveDaily(const std::vector<std::pair<std::string,int>>& limits) {
    auto conn = http::db::DbConnectionPool::getInstance().getConnection();
    conn->transaction([&](sql::Connection& connection) {
        std::unique_ptr<sql::PreparedStatement> clock(connection.prepareStatement("SELECT UTC_DATE() AS day, TIMESTAMPDIFF(SECOND, UTC_TIMESTAMP(), UTC_DATE()+INTERVAL 1 DAY) AS retry"));
        std::unique_ptr<sql::ResultSet> now(clock->executeQuery()); now->next();
        const std::string day = now->getString("day"); const int retry = now->getInt("retry"); now.reset();
        for (const auto& item : limits) {
            std::unique_ptr<sql::PreparedStatement> insert(connection.prepareStatement("INSERT IGNORE INTO usage_daily(day,scope,used) VALUES (?,?,0)"));
            insert->setString(1,day); insert->setString(2,item.first); insert->executeUpdate();
            std::unique_ptr<sql::PreparedStatement> update(connection.prepareStatement("UPDATE usage_daily SET used=used+1 WHERE day=? AND scope=? AND used<?"));
            update->setString(1,day); update->setString(2,item.first); update->setInt(3,item.second);
            if (update->executeUpdate() != 1) throw AccessError(429,"今天的使用或注册额度已用完，请明天再试（UTC 零点重置）",std::max(1,retry));
        }
    });
}
void PublicAccess::reserveRegistration(const std::string& ip) {
    reserveDaily({{"register-site",registrationsDaily_},{"register-ip:"+ip,registrationsIpDaily_}});
}
std::shared_ptr<PublicAccess::Permit> PublicAccess::admit(int uid, const std::string& ip) {
    std::lock_guard<std::mutex> lock(activeMutex_);
    if (activeUsers_.count(uid)) throw AccessError(409,"请先停止或等待当前回复完成");
    if (activeUsers_.size() >= static_cast<size_t>(concurrent_)) throw AccessError(503,"当前使用人数较多，请稍后重试",5);
    rate("chat-user:"+std::to_string(uid),perMinute_,60);
    rate("chat-ip:"+ip,30,60);
    // Reserve before contacting the provider. Failure/cancellation still counts,
    // since providers may charge for partial or failed requests.
    reserveDaily({{"chat-site",siteDaily_},{"chat-user:"+std::to_string(uid),userDaily_}});
    auto permit = std::make_shared<Permit>(this,uid);
    activeUsers_.insert(uid); return permit;
}
PublicAccess::Permit::~Permit() {
    std::lock_guard<std::mutex> lock(owner->activeMutex_); owner->activeUsers_.erase(userId);
}
json PublicAccess::usage(int uid) {
    auto result = mysql_.executeQuery("SELECT scope,used FROM usage_daily WHERE day=UTC_DATE() AND scope IN ('chat-site',?)","chat-user:"+std::to_string(uid));
    int user = 0, site = 0;
    while (result->next()) {
        if (result->getString("scope") == "chat-site") site = result->getInt("used"); else user = result->getInt("used");
    }
    return {{"dailyLimit",userDaily_},{"used",user},{"remaining",std::max(0,std::min(userDaily_-user,siteDaily_-site))},{"resetTimezone","UTC"}};
}
json PublicAccess::config() const {
    return {{"registrationOpen",registration_},{"models",models_},{"speechEnabled",speech_},
        {"dailyLimit",userDaily_},{"passwordMinLength",15},{"passwordMaxLength",128}};
}
