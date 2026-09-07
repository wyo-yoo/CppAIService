#pragma once
#include "router/RouterHandler.h"
#include "utils/MysqlUtil.h"
#include "../ChatServer.h"

class ChatSessionsHandler : public http::router::RouterHandler
{
public:
    explicit ChatSessionsHandler(ChatServer* server) : server_(server) {}

    void handle(const http::HttpRequest& req, http::HttpResponse* resp) override;
private:

private:
    ChatServer* server_;
    http::MysqlUtil     mysqlUtil_;
};
