#include "session/SessionManager.h"
#include <openssl/rand.h>
#include <array>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace http { namespace session {
SessionManager::SessionManager(std::unique_ptr<SessionStorage> storage, bool secureCookies)
    : storage_(std::move(storage)), secureCookies_(secureCookies) {}
std::shared_ptr<Session> SessionManager::getSession(const HttpRequest& req, HttpResponse*) {
    auto session = storage_->load(getSessionIdFromCookie(req));
    if (session && !session->isExpired()) { session->refresh(); return session; }
    // Anonymous traffic does not allocate persistent server-side sessions.
    return std::make_shared<Session>("", nullptr);
}
std::shared_ptr<Session> SessionManager::createSession(const HttpRequest& req, HttpResponse* resp) {
    logout(req, resp);
    auto id = generateSessionId();
    auto session = std::make_shared<Session>(id, this);
    storage_->save(session); setSessionCookie(id, resp); return session;
}
void SessionManager::logout(const HttpRequest& req, HttpResponse* resp) {
    auto id = getSessionIdFromCookie(req);
    if (auto session = storage_->load(id)) session->clear();
    storage_->remove(id);
    resp->addHeader("Set-Cookie", std::string("sessionId=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0") + (secureCookies_ ? "; Secure" : ""));
}
std::string SessionManager::generateSessionId() {
    std::array<unsigned char,32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw std::runtime_error("Cannot create secure session");
    std::string id; const char* digits = "0123456789abcdef";
    for (auto byte : bytes) { id += digits[byte >> 4]; id += digits[byte & 15]; }
    return id;
}
void SessionManager::destroySession(const std::string& id) { storage_->remove(id); }
void SessionManager::cleanExpiredSessions() {}
std::string SessionManager::getSessionIdFromCookie(const HttpRequest& req) {
    std::istringstream input(req.getHeader("Cookie")); std::string part;
    while (std::getline(input,part,';')) {
        part.erase(0,part.find_first_not_of(" \t"));
        if (part.compare(0,10,"sessionId=") != 0) continue;
        auto id = part.substr(10);
        if (id.size() == 64 && std::all_of(id.begin(),id.end(),[](char c){ return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return id;
    }
    return {};
}
void SessionManager::setSessionCookie(const std::string& id, HttpResponse* resp) {
    resp->addHeader("Set-Cookie", "sessionId=" + id + "; Path=/; HttpOnly; SameSite=Lax" + (secureCookies_ ? "; Secure" : ""));
}
} }
