#include "http/HttpServer.h"
#include "AIUtil/ChatTaskPool.h"
#include <chrono>
#include <thread>
#include <atomic>

int main(int argc, char** argv) {
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    http::HttpServer server(argc > 1 ? std::stoi(argv[1]) : 18991, "chat-test");
    ChatTaskPool workers(2, 4);
    std::atomic<int> disconnected{0};
    server.Get("/health", [&](const http::HttpRequest&, http::HttpResponse* response) {
        response->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        response->setBody(std::to_string(disconnected.load()));
        response->setContentLength(std::to_string(disconnected.load()).size());
    });
    server.Post("/stream", [&](const http::HttpRequest&, http::HttpResponse* response) {
        response->setStatusLine("HTTP/1.1", http::HttpResponse::k200Ok, "OK");
        response->setStreamHandler([&](std::shared_ptr<http::ResponseStream> stream) {
            workers.submit([&, stream] {
                stream->send("meta", "{\"sessionId\":\"1\"}");
                for (int i=0; i<20; ++i) {
                    if (stream->closed()) { ++disconnected; return; }
                    stream->send("delta", "{\"text\":\"你好\"}");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                stream->send("done", "{\"stopped\":false}"); stream->finish();
            });
        });
    });
    server.start();
}
