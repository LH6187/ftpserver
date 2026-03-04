#pragma once
#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>

using namespace std;

// 全局配置结构 (使用 extern 以便在其他文件访问)
struct ServerConfig {
    string listenIP;      // 监听 IP，默认 "0.0.0.0"
    int listenPort;       // 监听端口，默认 21
    bool isDirty;         // 标记配置是否被修改，需要重启
};

// 声明全局配置实例
extern ServerConfig g_ServerConfig;

namespace ConfigModule {
    // 初始化默认配置
    void initConfig();

    // 显示当前配置
    void showCurrentConfig();

    // 交互式设置监听地址
    // 返回 true 表示设置成功，false 表示用户取消或输入错误
    bool setListenAddress();

    // 验证 IP 地址是否合法
    bool isValidIPv4(const string& ip);
}