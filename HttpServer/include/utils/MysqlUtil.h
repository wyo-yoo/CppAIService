 #pragma once
 #include "db/DbConnectionPool.h"
 
#include <string>

namespace http
{

class MysqlUtil
{
public:
    static void init(const std::string& host, const std::string& user,
                    const std::string& password, const std::string& database,
                    size_t poolSize = 10)
    {
        http::db::DbConnectionPool::getInstance().init(
            host, user, password, database, poolSize);
    }

    template<typename... Args>
    std::shared_ptr<sql::ResultSet> executeQuery(const std::string& sql, Args&&... args)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        struct QueryLease {
            std::shared_ptr<http::db::DbConnection> connection;
            std::shared_ptr<sql::ResultSet> result;
        };
        auto lease = std::make_shared<QueryLease>();
        lease->connection = conn;
        lease->result = conn->executeQuery(sql, std::forward<Args>(args)...);
        return std::shared_ptr<sql::ResultSet>(lease, lease->result.get());
    }

    template<typename... Args>
    int executeUpdate(const std::string& sql, Args&&... args)
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        return conn->executeUpdate(sql, std::forward<Args>(args)...);
    }
};

} // namespace http
