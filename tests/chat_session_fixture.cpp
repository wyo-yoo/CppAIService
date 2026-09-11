// This fixture uses the production ChatServer/routes and a real disposable MySQL
// database. Only the model endpoint and MQ delivery are replaced for isolation.
#include "ChatServer.h"
#include "AIUtil/MQManager.h"
#include <cppconn/statement.h>

MQManager::MQManager(size_t size) : poolSize_(size), counter_(0) {}
void MQManager::publish(const std::string&, const std::string& payload) {
    auto m=json::parse(payload);
    http::MysqlUtil().executeUpdate(
        "INSERT INTO chat_message (id,username,session_id,is_user,content,ts) SELECT ?,?,?,?,?,? WHERE NOT EXISTS (SELECT 1 FROM chat_sessions WHERE user_id=? AND session_id=? AND deleted=1)",
        m.at("userId").get<int>(), m.at("username").get<std::string>(), m.at("sessionId").get<std::string>(),
        m.at("isUser").get<bool>(), m.at("content").get<std::string>(), m.at("timestamp").get<long long>(),
        m.at("userId").get<int>(), m.at("sessionId").get<std::string>());
}
class TestModel : public AliyunStrategy {
public:
    explicit TestModel(std::string url) : url_(std::move(url)) {}
    std::string getApiUrl() const override { return url_; }
private: std::string url_;
};
class TestDeepSeekModel : public DeepSeekStrategy {
public:
    explicit TestDeepSeekModel(std::string url) : url_(std::move(url)) {}
    std::string getApiUrl() const override { return url_; }
private:
    std::string url_;
};
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    std::string dbUrl="tcp://127.0.0.1:"+std::string(argv[2]);
    std::unique_ptr<sql::Connection> connection(sql::mysql::get_mysql_driver_instance()->connect(dbUrl,"root",""));
    std::unique_ptr<sql::Statement> statement(connection->createStatement());
    statement->execute("CREATE DATABASE IF NOT EXISTS chat_feature_test CHARACTER SET utf8mb4");
    connection->setSchema("chat_feature_test");
    statement->execute("CREATE TABLE IF NOT EXISTS users (id INT PRIMARY KEY AUTO_INCREMENT,username VARCHAR(255) UNIQUE,password VARCHAR(255))");
    statement->execute("CREATE TABLE IF NOT EXISTS chat_message (id INT,username VARCHAR(255),session_id VARCHAR(64),is_user TINYINT,content MEDIUMTEXT,ts BIGINT)");
    http::MysqlUtil::init(dbUrl,"root","","chat_feature_test",5);
    setenv("DASHSCOPE_API_KEY","test-only",1);
    setenv("DEEPSEEK_API_KEY","test-only-deepseek",1);
    setenv("DEEPSEEK_MODEL","deepseek-flash",1);
    const std::string endpoint=argv[3];
    StrategyFactory::instance().registerStrategy("1",[endpoint] { return std::make_shared<TestModel>(endpoint); });
    StrategyFactory::instance().registerStrategy("5",[endpoint] { return std::make_shared<TestDeepSeekModel>(endpoint); });
    ChatServer server(std::stoi(argv[1]),"session-test");
    server.initChatMessage();
    server.setThreadNum(2);
    server.start();
}
