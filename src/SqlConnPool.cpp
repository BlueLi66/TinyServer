#include "../include/SqlConnPool.h"
#include "../include/Logger.h"

RaiiConn::RaiiConn(SqlConnPool& pool) : pool_(pool) {
    conn_ = pool_.GetConn();
}

RaiiConn::~RaiiConn() {
    pool_.FreeConn(conn_);
}

SqlConnPool& SqlConnPool::Instance() {
    static SqlConnPool instance;
    return instance;
}

SqlConnPool::SqlConnPool() {

}

SqlConnPool::~SqlConnPool() {
    ClosePool();
}

void SqlConnPool::Init(const std::string& host, int port, const std::string& user, const std::string& passwd, const std::string& dbname, int max_conn) {
    if (is_inited_) {
        ClosePool();
    }
    is_inited_ = true;
    max_conn_ = max_conn;
    sem_init(&sem_, 0, max_conn);

    for (int i = 0; i < max_conn; ++i) {
        MYSQL* conn = mysql_init(NULL);
        if (mysql_real_connect(conn, host.c_str(), 
                               user.c_str(), passwd.c_str(), 
                               dbname.c_str(), port, NULL, 0) == NULL){
            LOG_ERROR("MySQL连接失败: " + std::string(mysql_error(conn)));
            mysql_close(conn);
            continue;
        }
        conn_queue_.push(conn);
    }
    LOG_INFO("数据库连接池初始化完成，连接数: " + std::to_string(conn_queue_.size()));
}

MYSQL* SqlConnPool::GetConn() {
    sem_wait(&sem_);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        MYSQL* conn = conn_queue_.front();
        conn_queue_.pop();
        return conn;
    }
}

void SqlConnPool::FreeConn(MYSQL* conn) {
    {
    std::lock_guard<std::mutex> lock(mtx_);
    conn_queue_.push(conn);
    }
    sem_post(&sem_);

}

void SqlConnPool::ClosePool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!conn_queue_.empty()) {
            MYSQL* conn = conn_queue_.front();
            conn_queue_.pop();
            mysql_close(conn);
        }
       
    }
    sem_destroy(&sem_);
    is_inited_ = false;
}

int SqlConnPool::AvailableCount() {
    std::lock_guard<std::mutex> lock(mtx_);
    return conn_queue_.size();
}