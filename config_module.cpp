// config_module.cpp
#define NOMINMAX
#include "config_module.h"
#include "db_log_module.h"
#include <iostream>
#include <limits>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace std;

// 定义全局配置实例
ServerConfig g_ServerConfig;

namespace ConfigModule {

    void initConfig() {
        // 从数据库加载配置
        ServerDbConfig dbConfig = DbLogModule::getCurrentConfig();

        g_ServerConfig.listenIP = dbConfig.listenIP;
        g_ServerConfig.listenPort = dbConfig.listenPort;
        g_ServerConfig.isDirty = false;

        // 注释掉这行，避免重复打印
        // cout << "从数据库加载配置: " << g_ServerConfig.listenIP << ":" << g_ServerConfig.listenPort << endl;
    }

    bool isValidIPv4(const string& ip) {
        sockaddr_in sa;
        int result = inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr));
        return (result == 1);
    }

    void showCurrentConfig() {
        cout << endl;
        cout << "--- Current Server Configuration ---" << endl;
        cout << "Listen Address: " << g_ServerConfig.listenIP << endl;
        cout << "Listen Port:    " << g_ServerConfig.listenPort << endl;
        if (g_ServerConfig.isDirty) {
            cout << "[!] Status: Configuration changed. RESTART REQUIRED." << endl;
        }
        else {
            cout << "[*] Status: Running with current settings." << endl;
        }
        cout << "------------------------------------" << endl;
    }

    bool setListenAddress() {
        cout << endl;
        cout << "=== Set Server Listen Address ===" << endl;
        cout << "Current IP: " << g_ServerConfig.listenIP << endl;
        cout << "Enter new IP (e.g., 192.168.1.5 or 0.0.0.0):" << endl;
        cout << "Type 'c' to cancel." << endl;
        cout << "> ";

        string input;
        if (!(cin >> input)) {
            cin.clear();
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            return false;
        }

        if (input == "c" || input == "C") {
            cout << "Operation cancelled." << endl;
            return false;
        }

        if (!isValidIPv4(input)) {
            cout << "Error: Invalid IPv4 address format!" << endl;
            return false;
        }

        // 更新内存中的配置
        g_ServerConfig.listenIP = input;
        g_ServerConfig.isDirty = true;

        // 将新IP地址写入数据库
        if (DbLogModule::updateListenIP(input)) {
            cout << "Success! New IP set to: " << input << endl;
            cout << "*** NOTE: You must restart the server (Option 0) for changes to take effect. ***" << endl;
        }
        else {
            cout << "Error: Failed to save IP address to database!" << endl;
        }

        // 清理输入缓冲区
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        return true;
    }
}