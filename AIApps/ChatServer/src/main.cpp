#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include"../include/ChatServer.h"

const std::string RABBITMQ_HOST = "localhost";
const std::string QUEUE_NAME = "sql_queue";
const int THREAD_NUM = 2;

void executeMysql(const std::string sql) {
    http::MysqlUtil mysqlUtil_;
    if (!sql.empty() && sql.front() == '{') {
        auto message = json::parse(sql);
        if (message.at("type") != "chat_message_v2") throw std::runtime_error("Unknown queue message type");
        int uid = message.at("userId").get<int>();
        auto sid = message.at("sessionId").get<std::string>();
        mysqlUtil_.executeUpdate(
            "INSERT INTO chat_message (id,username,session_id,is_user,content,ts) SELECT ?,?,?,?,?,? WHERE NOT EXISTS (SELECT 1 FROM chat_sessions WHERE user_id=? AND session_id=? AND deleted=1)",
            uid, message.at("username").get<std::string>(), sid, message.at("isUser").get<bool>(),
            message.at("content").get<std::string>(), message.at("timestamp").get<long long>(), uid, sid);
    } else {
        // Drain SQL jobs published by the previous version during upgrades.
        mysqlUtil_.executeUpdate(sql);
    }
}


int main(int argc, char* argv[]) {
	LOG_INFO << "pid = " << getpid();
	std::string serverName = "ChatServer";
	int port = 80;
    // 
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            port = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    ChatServer server(port, serverName);
    server.setThreadNum(4);
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    server.initChatMessage();    


    RabbitMQThreadPool pool(RABBITMQ_HOST, QUEUE_NAME, THREAD_NUM, executeMysql);
    pool.start();

    server.start();
}
