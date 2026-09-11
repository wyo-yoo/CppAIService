#include "security/Password.h"
#include "security/PublicAccess.h"
#include "session/SessionManager.h"
#include "http/HttpContext.h"
#include <cassert>
#include <iostream>

http::HttpRequest request(const std::string& headers = "", const std::string& method = "GET") {
    muduo::net::Buffer buffer;
    buffer.append(method+" /login HTTP/1.1\r\nHost: localhost\r\n"+headers+"\r\n");
    http::HttpContext context;
    assert(context.parseRequest(&buffer,muduo::Timestamp::now()) && context.gotAll());
    auto req = context.request(); req.setPeerIp("203.0.113.10"); return req;
}
std::string wire(const http::HttpResponse& response) {
    muduo::net::Buffer buffer; response.appendToBuffer(&buffer); return buffer.retrieveAllAsString();
}
int main() {
    auto a = Password::hash("a long test passphrase"), b = Password::hash("a long test passphrase");
    assert(a != b && Password::isEncoded(a));
    assert(Password::verify("a long test passphrase",a));
    assert(!Password::verify("a wrong test passphrase",a));
    assert(!Password::verify("a long test passphrase","plaintext"));
    assert(!Password::verify("a long test passphrase",a+"x"));
    assert(!Password::verify("a long test passphrase","pbkdf2_sha256$999999999$bad$bad"));
    assert(!Password::validNew("short"));
    assert(Password::validNew("记住一段足够长而且容易输入的安全密码句子"));
    assert(!Password::validNew(std::string(129,'a')));

    http::session::SessionManager sessions(std::make_unique<http::session::MemorySessionStorage>(),true);
    http::HttpResponse response;
    auto anonymous = request();
    assert(sessions.getSession(anonymous,&response)->getId().empty());
    assert(wire(response).find("Set-Cookie") == std::string::npos);
    auto session = sessions.createSession(anonymous,&response);
    session->setValue("isLoggedIn","true");
    assert(session->getId().size() == 64);
    assert(wire(response).find("HttpOnly; SameSite=Lax; Secure") != std::string::npos);
    auto cookie = request("Cookie: sessionId="+session->getId()+"\r\n");
    assert(sessions.getSession(cookie,&response)->getValue("isLoggedIn") == "true");
    auto fake = request("Cookie: othersessionId="+session->getId()+"\r\n");
    assert(sessions.getSession(fake,&response)->getId().empty());
    auto fresh = sessions.createSession(cookie,&response);
    assert(fresh->getId() != session->getId());
    assert(sessions.getSession(cookie,&response)->getId().empty());
    auto newCookie = request("Cookie: sessionId="+fresh->getId()+"\r\n");
    sessions.logout(newCookie,&response);
    assert(sessions.getSession(newCookie,&response)->getId().empty());
    assert(wire(response).find("Max-Age=0") != std::string::npos);

    unsetenv("CHAT_PUBLIC_ORIGIN"); unsetenv("CHAT_TRUST_PROXY");
    PublicAccess access;
    auto spoof = request("X-Real-IP: 198.51.100.9\r\nX-Forwarded-For: 198.51.100.9\r\n");
    assert(access.clientIp(spoof) == "203.0.113.10");
    auto cross = request("Content-Type: application/json\r\nContent-Length: 0\r\nOrigin: https://evil.example\r\n","POST");
    try { access.before(cross); assert(false); } catch (const http::HttpResponse& error) { assert(error.getStatusCode()==403); }
    for (int i=0;i<10;++i) access.checkLogin("ALICE");
    try { access.checkLogin("alice"); assert(false); } catch (const AccessError& error) { assert(error.status==429 && error.retryAfter>0); }

    setenv("CHAT_TRUST_PROXY","1",1);
    try { PublicAccess invalid; assert(false); } catch (const std::runtime_error&) {}
    unsetenv("CHAT_TRUST_PROXY");
    std::cout << "password hashing, random sessions, rotation/logout, CSRF and login throttling passed\n";
}
