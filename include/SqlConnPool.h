#pragma once

#include <mysql/mysql.h>
#include <queue>
#include <semaphore.h>
#include <mutex>
#include <string>

class SqlConnPool;

class RaiiConn{
public:
    RaiiConn(SqlConnPool& pool);
    ~RaiiConn();

    MYSQL* get() const { return conn_; }

    RaiiConn(const RaiiConn&) = delete;
    RaiiConn& operator=(const RaiiConn&) = delete;
private:
    MYSQL* conn_;
    SqlConnPool& pool_;
};

class SqlConnPool {
public:
    static SqlConnPool& Instance();
    void Init(const std::string& host, int port,
              const std::string& user, const std::string& passwd,
              const std::string& dbname, int max_conn = 8);
    MYSQL* GetConn();
    void FreeConn(MYSQL* conn);
    void ClosePool();
    int AvailableCount();

private:
    SqlConnPool();
    ~SqlConnPool();
    SqlConnPool(const SqlConnPool&) = delete;
    SqlConnPool& operator=(const SqlConnPool&) = delete;

    std::queue<MYSQL*> conn_queue_; // 空闲连接队列
    std::mutex mtx_;    // 保护 conn_queue_
    sem_t sem_; // 信号量 = 空闲连接数
    int max_conn_ = 0;
    bool is_inited_ = false;
};