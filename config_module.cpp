#define NOMINMAX
#include "config_module.h"
#include <iostream>
#include <limits>
#include <winsock2.h>
#include <ws2tcpip.h>


using namespace std;

// 定义全局配置实例
ServerConfig g_ServerConfig;

namespace ConfigModule {

    void initConfig() {
        g_ServerConfig.listenIP = "0.0.0.0";
        g_ServerConfig.listenPort = 21;
        g_ServerConfig.isDirty = false;
    }

    bool isValidIPv4(const string& ip) {
        sockaddr_in sa;
        // 使用 inet_pton 验证 IP 格式
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

        g_ServerConfig.listenIP = input;
        g_ServerConfig.isDirty = true;

        cout << "Success! New IP set to: " << input << endl;
        cout << "*** NOTE: You must restart the server (Option 0) for changes to take effect. ***" << endl;

        // 清理缓冲区
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        return true;
    }
}