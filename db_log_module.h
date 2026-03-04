// db_log_module.h
#ifndef DB_LOG_MODULE_H
#define DB_LOG_MODULE_H

#include <string>
#include <vector>
#include <ctime>

// 日志操作类型
enum DbLogOperation {
    DB_LOG_UPLOAD,
    DB_LOG_DOWNLOAD,
    DB_LOG_DELETE,
    DB_LOG_LOGIN,
    DB_LOG_LOGOUT,
    DB_LOG_SERVER_START,
    DB_LOG_SERVER_STOP,
    DB_LOG_CONNECT,
    DB_LOG_DISCONNECT
};

// 服务器配置结构
struct ServerDbConfig {
    std::string listenIP;
    int listenPort;
    std::string rootDirectory;
    bool autoStart;
    int maxConnections;
    int timeoutSeconds;

    // 默认构造函数
    ServerDbConfig()
        : listenIP("0.0.0.0")
        , listenPort(21)
        , rootDirectory("D:\\FtpRoot")
        , autoStart(false)
        , maxConnections(100)
        , timeoutSeconds(300) {
    }
};

// 日志记录结构
struct DbLogRecord {
    int id;
    std::string timestamp;
    std::string clientIP;
    std::string operation;
    std::string filename;
    std::string status;
    long fileSize;
    std::string details;
};



class DbLogModule {
public:
    static bool initialize(const std::string& dbPath = "ftp_server.db");
    static void shutdown();

    // 记录日志
    static bool recordLog(const std::string& clientIP, DbLogOperation op,
        const std::string& filename, const std::string& status,
        long fileSize, const std::string& details = "");

    static void recordTransfer(const std::string& clientIP, const std::string& type,
        const std::string& filename, long size);

    // 日志查询
    static std::vector<DbLogRecord> queryLatestLogs(int limit = 100);
    static std::vector<DbLogRecord> queryLogsByClient(const std::string& clientIP, int limit = 100);
    static std::vector<DbLogRecord> queryLogsByOperation(DbLogOperation op, int limit = 100);
    static std::string getStatistics();
    static bool clearAllLogs();
    static std::string getOperationName(DbLogOperation op);

    // ==================== 配置管理函数 ====================
    static bool initConfigTable();
    static bool loadConfig(ServerDbConfig& config);
    static bool saveConfig(const ServerDbConfig& config);
    static bool updateListenIP(const std::string& newIP);
    static bool updateListenPort(int newPort);
    static ServerDbConfig getCurrentConfig();

   

private:
    static void* m_db;
    static bool m_initialized;
    static bool createTables();
    static bool createConfigTable();
    static std::string getCurrentTimestamp();
};

#endif