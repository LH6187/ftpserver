// SharedConfig.h
#ifndef SHARED_CONFIG_H
#define SHARED_CONFIG_H

#include <string>
#include <fstream>
#include <direct.h>
#include <windows.h>

// 服务器配置结构体
struct ServerConfig {
    std::string listenIP;      // 监听IP地址
    int listenPort;            // 监听端口
    std::string rootDirectory; // 根目录
    bool autoStart;            // 是否自动启动
    int maxConnections;        // 最大连接数
    int timeoutSeconds;        // 超时时间（秒）
    bool isDirty;              // 配置是否已修改

    // 默认构造函数
    ServerConfig()
        : listenIP("0.0.0.0")
        , listenPort(21)
        , rootDirectory("C:\\FtpRoot")
        , autoStart(false)
        , maxConnections(100)
        , timeoutSeconds(300)
        , isDirty(false)
    {
    }
};

// 配置文件路径
#define CONFIG_FILE "server_config.ini"

// 保存配置到文件
inline bool SaveConfigToFile(const ServerConfig& config) {
    std::ofstream file(CONFIG_FILE);
    if (!file.is_open()) {
        return false;
    }

    file << "listenIP=" << config.listenIP << std::endl;
    file << "listenPort=" << config.listenPort << std::endl;
    file << "rootDirectory=" << config.rootDirectory << std::endl;
    file << "autoStart=" << (config.autoStart ? "1" : "0") << std::endl;
    file << "maxConnections=" << config.maxConnections << std::endl;
    file << "timeoutSeconds=" << config.timeoutSeconds << std::endl;

    file.close();
    return true;
}

// 从文件加载配置
inline bool LoadConfigFromFile(ServerConfig& config) {
    std::ifstream file(CONFIG_FILE);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        if (key == "listenIP") {
            config.listenIP = value;
        }
        else if (key == "listenPort") {
            config.listenPort = std::stoi(value);
        }
        else if (key == "rootDirectory") {
            config.rootDirectory = value;
        }
        else if (key == "autoStart") {
            config.autoStart = (value == "1");
        }
        else if (key == "maxConnections") {
            config.maxConnections = std::stoi(value);
        }
        else if (key == "timeoutSeconds") {
            config.timeoutSeconds = std::stoi(value);
        }
    }

    file.close();
    return true;
}

// 声明全局配置变量
extern ServerConfig g_ServerConfig;

#endif // SHARED_CONFIG_H