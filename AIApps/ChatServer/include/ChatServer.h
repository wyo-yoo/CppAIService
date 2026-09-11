#pragma once

#include <atomic>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <mutex>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>


#include "http/HttpServer.h"
#include "utils/MysqlUtil.h"
#include "utils/FileUtil.h"
#include "utils/JsonUtil.h"
#include"AIUtil/AISpeechProcessor.h"
#include"AIUtil/AIHelper.h"
#include"AIUtil/base64.h"
#include"AIUtil/MQManager.h"
#include "AIUtil/ChatTaskPool.h"
#include "security/PublicAccess.h"


class ChatLoginHandler;
class ChatRegisterHandler;
class ChatLogoutHandler;
class ChatHandler;
class ChatEntryHandler;
class ChatSendHandler;
class ChatHistoryHandler;

class AIMenuHandler;


class ChatCreateAndSendHandler;
class ChatSessionsHandler;
class ChatSpeechHandler;

class ChatServer {
public:
	ChatServer(int port,
		const std::string& name,
		muduo::net::TcpServer::Option option = muduo::net::TcpServer::kNoReusePort);

	void setThreadNum(int numThreads);
	void start();
	void initChatMessage();
    ~ChatServer();
	// 发送异步 handler 的延迟响应（耗时接口的工作线程完成后调用，如语音合成）
	void sendDeferredResponse(const http::HttpResponse& resp)
	{
		httpServer_.sendDeferredResponse(resp);
	}
private:
	friend class ChatLoginHandler;
	friend class ChatRegisterHandler;
	friend  ChatLogoutHandler;
	friend class ChatHandler;
	friend class ChatEntryHandler;
	friend class ChatSendHandler;
	friend class AIMenuHandler;
	friend class ChatHistoryHandler;

	friend class ChatCreateAndSendHandler;
	friend class ChatSessionsHandler;
	friend class ChatSpeechHandler;

private:
	void initialize();
	void initializeSession();
	void initializeRouter();
    void initializeChatFeatures();
    void handleChatStream(const http::HttpRequest&, http::HttpResponse*, bool streaming = true, bool newSession = false);
    void handleAuthentication(const http::HttpRequest&, http::HttpResponse*, bool registration);
    void handleChatCancel(const http::HttpRequest&, http::HttpResponse*);
    void handleSessionRename(const http::HttpRequest&, http::HttpResponse*);
    void handleSessionDelete(const http::HttpRequest&, http::HttpResponse*);
    int authenticatedUser(const http::HttpRequest&, http::HttpResponse*, std::string* name = nullptr);
    void ensureSessionRecord(int userId, const std::string& id, const std::string& title);
	void initializeMiddleware();
	

	void readDataFromMySQL();

	void packageResp(const std::string& version, http::HttpResponse::HttpStatusCode statusCode,
		const std::string& statusMsg, bool close, const std::string& contentType,
		int contentLen, const std::string& body, http::HttpResponse* resp);

	void setSessionManager(std::unique_ptr<http::session::SessionManager> manager)
	{
		httpServer_.setSessionManager(std::move(manager));
	}
	http::session::SessionManager* getSessionManager() const
	{
		return httpServer_.getSessionManager();
	}

	http::HttpServer	httpServer_;

	http::MysqlUtil		mysqlUtil_;

    std::shared_ptr<PublicAccess> access_ = std::make_shared<PublicAccess>();
    ChatTaskPool authWorkers_{2,8};
    std::string dummyPasswordHash_;

	

	// std::unordered_map<int, std::shared_ptr<AIHelper>> chatInformation;

	std::unordered_map<int, std::unordered_map<std::string,std::shared_ptr<AIHelper> > > chatInformation;
	std::mutex	mutexForChatInformation;

	std::unordered_map<int,std::vector<std::string> > sessionsIdsMap;
	std::mutex mutexForSessionsId;

    struct ChatJob {
        int userId;
        std::string sessionId, requestId;
        std::atomic<bool> cancelled{false};
        std::shared_ptr<PublicAccess::Permit> permit;
    };
    std::unordered_map<int, std::unordered_map<std::string, std::string>> sessionNames_;
    std::mutex chatJobsMutex_;
    std::unordered_map<std::string, std::shared_ptr<ChatJob>> chatJobs_;
    ChatTaskPool chatWorkers_{4, 32};

};
