#include "handlers/ChatCreateAndSendHandler.h"
void ChatCreateAndSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) { server_->handleChatStream(req,resp,false,true); }
