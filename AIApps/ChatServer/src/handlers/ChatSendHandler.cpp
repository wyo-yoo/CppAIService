#include "handlers/ChatSendHandler.h"
void ChatSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) { server_->handleChatStream(req,resp,false,false); }
