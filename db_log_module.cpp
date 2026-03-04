// db_log_module.cpp
#include "db_log_module.h"
#include "sqlite3.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <windows.h>

using namespace std;

void* DbLogModule::m_db = nullptr;
bool DbLogModule::m_initialized = false;

bool DbLogModule::initialize(const std::string& dbPath)
{
    if (m_initialized) {
        return true;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc != SQLITE_OK) {
        cerr << "无法打开数据库: " << sqlite3_errmsg(db) << endl;
        return false;
    }

    m_db = db;
    m_initialized = true;

    // 创建所有表
    bool logTableCreated = createTables();
    bool configTableCreated = createConfigTable();

    // 初始化默认配置
    ServerDbConfig config;
    loadConfig(config);

    return true;
}

void DbLogModule::shutdown()
{
    if (m_db && m_initialized) {
        sqlite3_close(static_cast<sqlite3*>(m_db));
        m_db = nullptr;
        m_initialized = false;
    }
}

bool DbLogModule::createTables()
{
    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    const char* sql =
        "CREATE TABLE IF NOT EXISTS ftp_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp TEXT NOT NULL,"
        "client_ip TEXT NOT NULL,"
        "operation TEXT NOT NULL,"
        "filename TEXT,"
        "status TEXT NOT NULL,"
        "file_size INTEGER DEFAULT 0,"
        "details TEXT"
        ");";

    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "创建日志表失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    const char* indexSql =
        "CREATE INDEX IF NOT EXISTS idx_timestamp ON ftp_logs(timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_client_ip ON ftp_logs(client_ip);"
        "CREATE INDEX IF NOT EXISTS idx_operation ON ftp_logs(operation);";

    rc = sqlite3_exec(db, indexSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "创建索引失败: " << errMsg << endl;
        sqlite3_free(errMsg);
    }

    return true;
}

bool DbLogModule::createConfigTable()
{
    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    const char* sql =
        "CREATE TABLE IF NOT EXISTS server_config ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "listen_ip TEXT NOT NULL DEFAULT '0.0.0.0',"
        "listen_port INTEGER NOT NULL DEFAULT 21,"
        "root_directory TEXT NOT NULL DEFAULT 'D:\\FtpRoot',"
        "auto_start INTEGER NOT NULL DEFAULT 0,"
        "max_connections INTEGER NOT NULL DEFAULT 100,"
        "timeout_seconds INTEGER NOT NULL DEFAULT 300,"
        "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");";

    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "创建配置表失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}



bool DbLogModule::loadConfig(ServerDbConfig& config)
{
    if (!m_initialized || !m_db) {
        cerr << "数据库未初始化" << endl;
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;

    const char* sql = "SELECT listen_ip, listen_port, root_directory, auto_start, max_connections, timeout_seconds FROM server_config WHERE id = 1;";

    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        cerr << "准备查询配置失败: " << sqlite3_errmsg(db) << endl;
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        config.listenIP = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        config.listenPort = sqlite3_column_int(stmt, 1);
        config.rootDirectory = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        config.autoStart = (sqlite3_column_int(stmt, 3) != 0);
        config.maxConnections = sqlite3_column_int(stmt, 4);
        config.timeoutSeconds = sqlite3_column_int(stmt, 5);
    }
    else {
        const char* insertSql =
            "INSERT INTO server_config (listen_ip, listen_port, root_directory, auto_start, max_connections, timeout_seconds) "
            "VALUES ('0.0.0.0', 21, 'D:\\FtpRoot', 0, 100, 300);";

        sqlite3_exec(db, insertSql, nullptr, nullptr, nullptr);

        config = ServerDbConfig();
    }

    sqlite3_finalize(stmt);
    return true;
}

bool DbLogModule::saveConfig(const ServerDbConfig& config)
{
    if (!m_initialized || !m_db) {
        cerr << "数据库未初始化" << endl;
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    stringstream sql;
    sql << "UPDATE server_config SET "
        << "listen_ip = '" << config.listenIP << "', "
        << "listen_port = " << config.listenPort << ", "
        << "root_directory = '" << config.rootDirectory << "', "
        << "auto_start = " << (config.autoStart ? 1 : 0) << ", "
        << "max_connections = " << config.maxConnections << ", "
        << "timeout_seconds = " << config.timeoutSeconds << ", "
        << "updated_at = CURRENT_TIMESTAMP "
        << "WHERE id = 1;";

    int rc = sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "保存配置失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool DbLogModule::updateListenIP(const std::string& newIP)
{
    if (!m_initialized || !m_db) {
        cerr << "数据库未初始化" << endl;
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    stringstream sql;
    sql << "UPDATE server_config SET listen_ip = '" << newIP << "', updated_at = CURRENT_TIMESTAMP WHERE id = 1;";

    int rc = sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "更新监听IP失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    cout << "监听IP已更新为: " << newIP << " (需要重启服务器生效)" << endl;
    return true;
}

bool DbLogModule::updateListenPort(int newPort)
{
    if (!m_initialized || !m_db) {
        cerr << "数据库未初始化" << endl;
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    stringstream sql;
    sql << "UPDATE server_config SET listen_port = " << newPort << ", updated_at = CURRENT_TIMESTAMP WHERE id = 1;";

    int rc = sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        cerr << "更新监听端口失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    cout << "监听端口已更新为: " << newPort << " (需要重启服务器生效)" << endl;
    return true;
}

ServerDbConfig DbLogModule::getCurrentConfig()
{
    ServerDbConfig config;
    loadConfig(config);
    return config;
}

std::string DbLogModule::getCurrentTimestamp()
{
    time_t now = time(nullptr);
    tm ltm;
    localtime_s(&ltm, &now);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &ltm);
    return std::string(buffer);
}

std::string DbLogModule::getOperationName(DbLogOperation op)
{
    switch (op) {
    case DB_LOG_UPLOAD: return "UPLOAD";
    case DB_LOG_DOWNLOAD: return "DOWNLOAD";
    case DB_LOG_DELETE: return "DELETE";
    case DB_LOG_LOGIN: return "LOGIN";
    case DB_LOG_LOGOUT: return "LOGOUT";
    case DB_LOG_SERVER_START: return "SERVER_START";
    case DB_LOG_SERVER_STOP: return "SERVER_STOP";
    case DB_LOG_CONNECT: return "CONNECT";
    case DB_LOG_DISCONNECT: return "DISCONNECT";
    default: return "UNKNOWN";
    }
}

bool DbLogModule::recordLog(const std::string& clientIP, DbLogOperation op,
    const std::string& filename, const std::string& status,
    long fileSize, const std::string& details)
{
    if (!m_initialized || !m_db) {
        cerr << "数据库未初始化" << endl;
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    int rc;

    std::string timestamp = getCurrentTimestamp();
    std::string operation = getOperationName(op);

    cout << "尝试插入日志 - 时间: " << timestamp
        << ", IP: " << clientIP
        << ", 操作: " << operation
        << ", 文件: " << filename
        << ", 状态: " << status
        << ", 大小: " << fileSize << endl;

    const char* sql =
        "INSERT INTO ftp_logs (timestamp, client_ip, operation, filename, status, file_size, details) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        cerr << "准备SQL语句失败, 错误码: " << rc << ", 错误信息: " << sqlite3_errmsg(db) << endl;
        return false;
    }

    // 绑定参数 - 使用 SQLITE_TRANSIENT 确保字符串数据安全
    sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, clientIP.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, operation.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, fileSize);
    sqlite3_bind_text(stmt, 7, details.c_str(), -1, SQLITE_TRANSIENT);

    // 执行插入
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {
        cerr << "插入日志失败, 错误码: " << rc << ", 错误信息: " << sqlite3_errmsg(db) << endl;
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);

    // 获取最后插入的ID
    long long lastId = sqlite3_last_insert_rowid(db);
    cout << "日志插入成功, ID: " << lastId << endl;

    return true;
}

void DbLogModule::recordTransfer(const std::string& clientIP, const std::string& type,
    const std::string& filename, long size)
{
    DbLogOperation op = (type == "UPLOAD") ? DB_LOG_UPLOAD : DB_LOG_DOWNLOAD;
    recordLog(clientIP, op, filename, "SUCCESS", size);
}

std::vector<DbLogRecord> DbLogModule::queryLatestLogs(int limit)
{
    std::vector<DbLogRecord> records;
    if (!m_initialized || !m_db) {
        return records;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;

    std::string sql = "SELECT id, timestamp, client_ip, operation, filename, status, file_size, details "
        "FROM ftp_logs ORDER BY timestamp DESC LIMIT ?;";

    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return records;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbLogRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.clientIP = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.operation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.fileSize = sqlite3_column_int(stmt, 6);
        record.details = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        records.push_back(record);
    }

    sqlite3_finalize(stmt);
    return records;
}

std::vector<DbLogRecord> DbLogModule::queryLogsByClient(const std::string& clientIP, int limit)
{
    std::vector<DbLogRecord> records;
    if (!m_initialized || !m_db) {
        return records;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;

    std::string sql = "SELECT id, timestamp, client_ip, operation, filename, status, file_size, details "
        "FROM ftp_logs WHERE client_ip = ? ORDER BY timestamp DESC LIMIT ?;";

    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return records;
    }

    sqlite3_bind_text(stmt, 1, clientIP.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbLogRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.clientIP = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.operation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.fileSize = sqlite3_column_int(stmt, 6);
        record.details = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        records.push_back(record);
    }

    sqlite3_finalize(stmt);
    return records;
}

std::vector<DbLogRecord> DbLogModule::queryLogsByOperation(DbLogOperation op, int limit)
{
    std::vector<DbLogRecord> records;
    if (!m_initialized || !m_db) {
        return records;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    std::string operation = getOperationName(op);

    std::string sql = "SELECT id, timestamp, client_ip, operation, filename, status, file_size, details "
        "FROM ftp_logs WHERE operation = ? ORDER BY timestamp DESC LIMIT ?;";

    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return records;
    }

    sqlite3_bind_text(stmt, 1, operation.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DbLogRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.clientIP = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        record.operation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        record.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        record.status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        record.fileSize = sqlite3_column_int(stmt, 6);
        record.details = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        records.push_back(record);
    }

    sqlite3_finalize(stmt);
    return records;
}

std::string DbLogModule::getStatistics()
{
    if (!m_initialized || !m_db) {
        return "数据库未初始化";
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    sqlite3_stmt* stmt = nullptr;
    stringstream result;

    const char* sql1 = "SELECT COUNT(*) FROM ftp_logs;";
    if (sqlite3_prepare_v2(db, sql1, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result << "总日志条数: " << sqlite3_column_int(stmt, 0) << "\n";
        }
        sqlite3_finalize(stmt);
    }

    const char* sql2 = "SELECT operation, COUNT(*) FROM ftp_logs GROUP BY operation;";
    if (sqlite3_prepare_v2(db, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* op = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            int count = sqlite3_column_int(stmt, 1);
            result << op << "次数: " << count << "\n";
        }
        sqlite3_finalize(stmt);
    }

    return result.str();
}

bool DbLogModule::clearAllLogs()
{
    if (!m_initialized || !m_db) {
        return false;
    }

    sqlite3* db = static_cast<sqlite3*>(m_db);
    char* errMsg = nullptr;

    const char* sql = "DELETE FROM ftp_logs;";
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        cerr << "清空日志失败: " << errMsg << endl;
        sqlite3_free(errMsg);
        return false;
    }

    sqlite3_exec(db, "DELETE FROM sqlite_sequence WHERE name='ftp_logs';", nullptr, nullptr, nullptr);
    return true;
}








