#include "handlers/ChatLoginHandler.h"
void ChatLoginHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) { server_->handleAuthentication(req,resp,false); }
