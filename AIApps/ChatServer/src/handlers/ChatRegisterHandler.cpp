#include "handlers/ChatRegisterHandler.h"
void ChatRegisterHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) { server_->handleAuthentication(req,resp,true); }
