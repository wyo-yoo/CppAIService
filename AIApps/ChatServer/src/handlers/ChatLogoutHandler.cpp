#include "handlers/ChatLogoutHandler.h"
void ChatLogoutHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) {
    server_->getSessionManager()->logout(req,resp);
    accessReply(resp,req,200,{{"success",true},{"message","已退出登录"}});
}
