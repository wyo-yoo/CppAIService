#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <type_traits>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql/mysql.h>
#include <muduo/base/Logging.h>
#include "DbException.h"

namespace http 
{
namespace db 
{

class DbConnection 
{
public:
    DbConnection(const std::string& host, 
                const std::string& user,
                const std::string& password,
                const std::string& database);
    ~DbConnection();

    // 禁止拷贝
    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    bool isValid();
    void reconnect();
    void cleanup();

    template<typename... Args>
    std::shared_ptr<sql::ResultSet> executeQuery(const std::string& sql, Args&&... args)
    {
        struct QueryOwner {
            explicit QueryOwner(std::mutex& mutex) : lock(mutex) {}
            std::unique_lock<std::mutex> lock;
            std::unique_ptr<sql::PreparedStatement> statement;
            std::unique_ptr<sql::ResultSet> result;
        };
        auto owner = std::make_shared<QueryOwner>(mutex_);
        try 
        {
            // 直接创建新的预处理语句，不使用缓存
            owner->statement.reset(conn_->prepareStatement(sql));
            bindParams(owner->statement.get(), 1, std::forward<Args>(args)...);
            owner->result.reset(owner->statement->executeQuery());
            return std::shared_ptr<sql::ResultSet>(owner, owner->result.get());
        } 
        catch (const sql::SQLException& e) 
        {
            LOG_ERROR << "Query failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }
    
    template<typename... Args>
    int executeUpdate(const std::string& sql, Args&&... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try 
        {
            // 直接创建新的预处理语句，不使用缓存
            std::unique_ptr<sql::PreparedStatement> stmt(
                conn_->prepareStatement(sql)
            );
            bindParams(stmt.get(), 1, std::forward<Args>(args)...);
            return stmt->executeUpdate();
        } 
        catch (const sql::SQLException& e) 
        {
            LOG_ERROR << "Update failed: " << e.what() << ", SQL: " << sql;
            throw DbException(e.what());
        }
    }

    bool ping();  // 添加检测连接是否有效的方法
    // Keep all quota rows on one leased connection and commit them together.
    template<typename Function>
    void transaction(Function&& function) {
        std::lock_guard<std::mutex> lock(mutex_);
        conn_->setAutoCommit(false);
        try {
            function(*conn_);
            conn_->commit();
            conn_->setAutoCommit(true);
        } catch (...) {
            try { conn_->rollback(); conn_->setAutoCommit(true); }
            catch (...) { try { conn_->close(); } catch (...) {} }
            throw;
        }
    }
private:
     // 辅助函数：递归终止条件
    void bindParams(sql::PreparedStatement*, int) {}
    
    // 辅助函数：绑定参数
    template<typename T, typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index, 
                   T&& value, Args&&... args) 
    {
        if constexpr (std::is_convertible_v<T, std::string>) stmt->setString(index, std::string(std::forward<T>(value)));
        else stmt->setString(index, std::to_string(std::forward<T>(value)));
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }
    
    // 特化 string 类型的参数绑定
    template<typename... Args>
    void bindParams(sql::PreparedStatement* stmt, int index, 
                   const std::string& value, Args&&... args) 
    {
        stmt->setString(index, value);
        bindParams(stmt, index + 1, std::forward<Args>(args)...);
    }

private:
    std::shared_ptr<sql::Connection> conn_;
    std::string                      host_;
    std::string                      user_;
    std::string                      password_;
    std::string                      database_;
    std::mutex                       mutex_;
};

} // namespace db
} // namespace http
