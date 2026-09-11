#pragma once
#include "middleware/Middleware.h"
#include "utils/MysqlUtil.h"
#include "utils/JsonUtil.h"
#include <chrono>
#include <map>
#include <set>
#include <mutex>
#include <stdexcept>

class AccessError : public std::runtime_error {
public:
    AccessError(int code, const std::string& message, int retry = 0) : std::runtime_error(message), status(code), retryAfter(retry) {}
    int status, retryAfter;
};
void accessReply(http::HttpResponse* response, const http::HttpRequest& request, int status, const json& body, int retry = 0);
int settingInt(const char* name, int fallback, int minimum, int maximum);

class PublicAccess : public http::middleware::Middleware {
public:
    PublicAccess();
    void initializeDatabase();
    void before(http::HttpRequest& request) override;
    void after(http::HttpResponse& response) override;
    std::string clientIp(const http::HttpRequest& request) const;
    void checkLogin(const std::string& username);
    void reserveRegistration(const std::string& ip);
    struct Permit {
        Permit(PublicAccess* access, int uid) : owner(access), userId(uid) {}
        ~Permit();
        PublicAccess* owner; int userId;
    };
    std::shared_ptr<Permit> admit(int userId, const std::string& ip);
    json usage(int userId);
    json config() const;
    bool modelAllowed(const std::string& model) const { return models_.count(model) != 0; }
    bool secureCookies() const { return !origin_.empty(); }
    bool speechEnabled() const { return speech_; }
private:
    void rate(const std::string& key, int limit, int seconds);
    void reserveDaily(const std::vector<std::pair<std::string, int>>& limits);
    struct Window { int count = 0; std::chrono::steady_clock::time_point expires; };
    http::MysqlUtil mysql_;
    std::string origin_;
    bool trustProxy_, registration_, speech_;
    int userDaily_, siteDaily_, concurrent_, perMinute_, registrationsDaily_, registrationsIpDaily_;
    std::set<std::string> models_;
    std::mutex ratesMutex_, activeMutex_;
    std::map<std::string, Window> windows_;
    std::set<int> activeUsers_;
};
