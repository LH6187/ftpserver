#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <direct.h>
#include <process.h>
#include <algorithm>
#include <sys/stat.h>
#include <codecvt>
#include <locale>
#include <thread>       // 新增：用于处理菜单输入的线程
#include "menu.h"
#include "log_module.h"
#include "config_module.h" // 新增

#pragma comment(lib, "ws2_32.lib")

using namespace std;

// 常量定义
#define BUFFER_SIZE 4096
#define FTP_PORT 21

// 辅助函数：将string转换为wstring
wstring stringToWstring(const string& str) {
    if (str.empty()) return wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstr[0], size_needed);
    return wstr;
}

// 辅助函数：将wstring转换为string
string wstringToString(const wstring& wstr) {
    if (wstr.empty()) return string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

// FTP会话类
class FtpSession {
private:
    SOCKET controlSocket;
    SOCKET dataSocket;
    string currentDirectory;
    string rootDirectory;
    string clientIPStr; // 新增成员
    bool authenticated;
    sockaddr_in clientAddr;

    // 辅助函数：将字符串转换为大写
    string toUpper(string str) {
        transform(str.begin(), str.end(), str.begin(), ::toupper);
        return str;
    }

    // 辅助函数：获取文件列表（类似ls -l格式）
    string getFileList(const string& path) {
        string result;
        WIN32_FIND_DATAW findData;
        wstring searchPath = stringToWstring(path + "\\*");
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                // 跳过当前目录和父目录
                if (wcscmp(findData.cFileName, L".") == 0 ||
                    wcscmp(findData.cFileName, L"..") == 0) {
                    continue;
                }

                // 将宽字符文件名转换为UTF-8
                string fileName = wstringToString(findData.cFileName);

                // 文件权限
                string permissions = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
                    "drwxr-xr-x" : "-rw-r--r--";

                // 文件大小
                LARGE_INTEGER fileSize;
                fileSize.HighPart = findData.nFileSizeHigh;
                fileSize.LowPart = findData.nFileSizeLow;

                // 格式化输出（类似ls -l格式）
                char line[512];
                SYSTEMTIME st;
                FileTimeToSystemTime(&findData.ftLastWriteTime, &st);

                sprintf_s(line, sizeof(line),
                    "%s 1 owner group %10lld %04d-%02d-%02d %02d:%02d %s\r\n",
                    permissions.c_str(),
                    fileSize.QuadPart,
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute,
                    fileName.c_str());

                result += line;
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        return result;
    }

public:
    FtpSession(SOCKET clientSock, sockaddr_in addr) {
        controlSocket = clientSock;
        clientAddr = addr;
        authenticated = true; // 简单实现，自动认证

        // 【新增】将二进制 IP 转换为字符串保存
        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipBuf, INET_ADDRSTRLEN);
        clientIPStr = string(ipBuf);

        // 设置根目录
        char currentPath[BUFFER_SIZE];
        _getcwd(currentPath, BUFFER_SIZE);
        rootDirectory = string(currentPath) + "\\FtpRoot";
        currentDirectory = rootDirectory;

        // 3. 创建 FTP 根目录 (如果不存在)
        // _mkdir 返回 0 表示成功或已存在，-1 表示失败
        if (_mkdir(rootDirectory.c_str()) == 0) {
            cout << "Created root directory: " << rootDirectory << endl;
        }

        // 【新增】4. 定义并创建子目录
        string publicKeyDir = rootDirectory + "\\PublicKey";
        string uploadDir = rootDirectory + "\\UpLoad";

        // 创建 PublicKey 文件夹
        if (_mkdir(publicKeyDir.c_str()) == 0) {
            cout << "Created subdirectory: PublicKey" << endl;
        }
        else {
            // 如果失败可能是因为已存在，可以忽略错误，或者打印提示
            cout << "Subdirectory PublicKey already exists or error." << endl;
        }

        // 创建 UpLoad 文件夹
        if (_mkdir(uploadDir.c_str()) == 0) {
            cout << "Created subdirectory: UpLoad" << endl;
        }
        else {
             cout << "Subdirectory UpLoad already exists or error." << endl;
        }
        dataSocket = INVALID_SOCKET;

        cout << "根目录: " << rootDirectory << endl;
    }

    void sendResponse(const string& response) {
        string msg = response + "\r\n";
        send(controlSocket, msg.c_str(), msg.length(), 0);
        cout << "发送: " << response << endl;
    }

    string receiveCommand() {
        char buffer[BUFFER_SIZE] = { 0 };
        int bytesReceived = recv(controlSocket, buffer, BUFFER_SIZE - 1, 0);

        if (bytesReceived <= 0) {
            return "";
        }

        string command(buffer);
        // 移除末尾的\r\n
        size_t pos = command.find("\r\n");
        if (pos != string::npos) {
            command = command.substr(0, pos);
        }
        return command;
    }

    void process() {
        sendResponse("220 Simple FTP Server Ready");

        while (true) {
            string commandLine = receiveCommand();
            if (commandLine.empty()) {
                break;
            }

            cout << "收到命令: " << commandLine << endl;
            processCommand(commandLine);
        }

        closesocket(controlSocket);
        if (dataSocket != INVALID_SOCKET) {
            closesocket(dataSocket);
        }
    }

    void processCommand(const string& commandLine) {
        // 解析命令和参数
        size_t spacePos = commandLine.find(' ');
        string cmd = (spacePos != string::npos) ?
            commandLine.substr(0, spacePos) : commandLine;
        string param = (spacePos != string::npos) ?
            commandLine.substr(spacePos + 1) : "";

        // 转换为大写命令
        cmd = toUpper(cmd);

        cout << "解析命令: " << cmd << ", 参数: " << param << endl;

        if (cmd == "USER") {
            sendResponse("331 User name okay, need password");
        }
        else if (cmd == "PASS") {
            sendResponse("230 User logged in");
        }
        else if (cmd == "PWD" || cmd == "XPWD") {
            string relativePath = getRelativePath(currentDirectory);
            sendResponse("257 \"" + relativePath + "\" is current directory");
        }
        else if (cmd == "CWD") {
            changeDirectory(param);
        }
        else if (cmd == "CDUP") {
            changeDirectory("..");
        }
        else if (cmd == "LIST" || cmd == "NLST") {
            listDirectory(param);
        }
        else if (cmd == "PORT") {
            setPort(param);
        }
        else if (cmd == "PASV") {
            sendResponse("502 Passive mode not implemented");
        }
        else if (cmd == "RETR") {
            downloadFile(param);
        }
        else if (cmd == "STOR") {
            uploadFile(param);
        }
        else if (cmd == "DELE") {
            deleteFile(param);
        }
        else if (cmd == "MKD") {
            makeDirectory(param);
        }
        else if (cmd == "RMD") {
            removeDirectory(param);
        }
        else if (cmd == "RNFR") {
            sendResponse("350 Ready for RNTO");
            // 简化实现，实际需要保存文件名
        }
        else if (cmd == "RNTO") {
            sendResponse("250 Rename successful");
        }
        else if (cmd == "TYPE") {
            if (param == "I" || param == "A") {
                sendResponse("200 Type set to " + param);
            }
            else {
                sendResponse("504 Command not implemented for that parameter");
            }
        }
        else if (cmd == "SYST") {
            sendResponse("215 Windows_NT");
        }
        else if (cmd == "FEAT") {
            sendResponse("211-Features:\r\n211 End");
        }
        else if (cmd == "NOOP") {
            sendResponse("200 NOOP command successful");
        }
        else if (cmd == "QUIT") {
            sendResponse("221 Goodbye");
            throw exception("Client disconnected");
        }
        else {
            sendResponse("502 Command not implemented: " + cmd);
        }
    }

    string getRelativePath(const string& path) {
        if (path.find(rootDirectory) == 0) {
            string relPath = path.substr(rootDirectory.length());
            // 替换反斜杠为正斜杠
            for (char& c : relPath) {
                if (c == '\\') c = '/';
            }
            if (relPath.empty() || relPath[0] != '/') {
                relPath = "/" + relPath;
            }
            return relPath;
        }
        return "/";
    }

    void changeDirectory(const string& path) {
        string newPath;

        if (path.empty()) {
            newPath = rootDirectory;
        }
        else if (path == "/") {
            newPath = rootDirectory;
        }
        else if (path == "..") {
            // 返回上一级目录，但不能超出根目录
            size_t pos = currentDirectory.find_last_of('\\');
            if (pos != string::npos && currentDirectory.length() > rootDirectory.length()) {
                newPath = currentDirectory.substr(0, pos);
            }
            else {
                newPath = rootDirectory;
            }
        }
        else if (path[0] == '/') {
            // 绝对路径
            newPath = rootDirectory + path;
            // 替换正斜杠为反斜杠
            for (char& c : newPath) {
                if (c == '/') c = '\\';
            }
        }
        else {
            // 相对路径
            newPath = currentDirectory + "\\" + path;
        }

        // 检查目录是否存在
        struct stat info;
        if (stat(newPath.c_str(), &info) == 0 && (info.st_mode & S_IFDIR)) {
            currentDirectory = newPath;
            sendResponse("250 Directory changed to " + getRelativePath(currentDirectory));
        }
        else {
            sendResponse("550 Directory not found: " + path);
        }
    }

    void setPort(const string& param) {
        // 解析PORT命令: h1,h2,h3,h4,p1,p2
        vector<string> parts;
        stringstream ss(param);
        string part;

        while (getline(ss, part, ',')) {
            parts.push_back(part);
        }

        if (parts.size() == 6) {
            string ip = parts[0] + "." + parts[1] + "." + parts[2] + "." + parts[3];
            int port = (stoi(parts[4]) << 8) + stoi(parts[5]);

            cout << "PORT命令: IP=" << ip << ", Port=" << port << endl;

            // 创建数据连接
            if (dataSocket != INVALID_SOCKET) {
                closesocket(dataSocket);
            }

            dataSocket = socket(AF_INET, SOCK_STREAM, 0);
            if (dataSocket == INVALID_SOCKET) {
                sendResponse("425 Can't create data socket");
                return;
            }

            sockaddr_in dataAddr;
            dataAddr.sin_family = AF_INET;
            dataAddr.sin_port = htons(port);

            // 使用 inet_pton 替代 inet_addr
            if (inet_pton(AF_INET, ip.c_str(), &dataAddr.sin_addr) <= 0) {
                sendResponse("425 Invalid IP address");
                closesocket(dataSocket);
                dataSocket = INVALID_SOCKET;
                return;
            }

            if (connect(dataSocket, (sockaddr*)&dataAddr, sizeof(dataAddr)) == 0) {
                sendResponse("200 Port command successful");
            }
            else {
                sendResponse("425 Can't open data connection");
                closesocket(dataSocket);
                dataSocket = INVALID_SOCKET;
            }
        }
        else {
            sendResponse("501 Syntax error in parameters");
        }
    }

    void listDirectory(const string& path) {
        if (dataSocket == INVALID_SOCKET) {
            sendResponse("425 No data connection");
            return;
        }

        sendResponse("150 Opening data connection for directory list");

        string targetPath = currentDirectory;
        if (!path.empty() && path != "-a" && path != "-l") {
            if (path[0] == '/') {
                targetPath = rootDirectory + path;
                for (char& c : targetPath) if (c == '/') c = '\\';
            }
            else {
                targetPath = currentDirectory + "\\" + path;
            }
        }

        // 获取文件列表
        string fileList = getFileList(targetPath);

        // 发送文件列表
        int totalSent = 0;
        int bytesToSend = fileList.length();

        while (totalSent < bytesToSend) {
            int sent = send(dataSocket, fileList.c_str() + totalSent,
                bytesToSend - totalSent, 0);
            if (sent <= 0) break;
            totalSent += sent;
        }

        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
        sendResponse("226 Directory send OK");
    }

    void downloadFile(const string& filename) {
        if (dataSocket == INVALID_SOCKET) {
            sendResponse("425 No data connection");
            return;
        }

        if (filename.empty()) {
            sendResponse("501 No file name specified");
            return;
        }

        string filepath = currentDirectory + "\\" + filename;
        ifstream file(filepath, ios::binary | ios::ate);

        if (!file) {
            sendResponse("550 File not found");
            return;
        }

        // 获取文件大小
        streampos fileSize = file.tellg();
        file.seekg(0, ios::beg);

        sendResponse("150 Opening data connection for file download (" +
            to_string(fileSize) + " bytes)");

        // 发送文件
        char buffer[BUFFER_SIZE];
        while (file.read(buffer, BUFFER_SIZE)) {
            int bytesRead = file.gcount();
            int totalSent = 0;
            while (totalSent < bytesRead) {
                int sent = send(dataSocket, buffer + totalSent, bytesRead - totalSent, 0);
                if (sent <= 0) break;
                totalSent += sent;
            }
        }
        // 发送最后一部分
        if (file.gcount() > 0) {
            send(dataSocket, buffer, file.gcount(), 0);
        }

        file.close();
        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
        sendResponse("226 File send OK");
        // 【新增】记录下载日志
        // 因为改成了普通 enum，直接使用枚举值
        LogModule::recordLog(clientIPStr, LOG_DOWNLOAD, filename, "SUCCESS", (long)fileSize);
    }

    void uploadFile(const string& filename) {
        if (dataSocket == INVALID_SOCKET) {
            sendResponse("425 No data connection");
            return;
        }

        if (filename.empty()) {
            sendResponse("501 No file name specified");
            return;
        }

        string filepath = currentDirectory + "\\" + filename;
        ofstream file(filepath, ios::binary);

        if (!file) {
            sendResponse("550 Can't create file");
            return;
        }

        sendResponse("150 Opening data connection for file upload");

        // 接收文件
        char buffer[BUFFER_SIZE];
        int bytesReceived;
        int totalBytes = 0;

        while ((bytesReceived = recv(dataSocket, buffer, BUFFER_SIZE, 0)) > 0) {
            file.write(buffer, bytesReceived);
            totalBytes += bytesReceived;
        }

        file.close();
        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
        sendResponse("226 File receive OK (" + to_string(totalBytes) + " bytes)");
        // 【新增】记录上传日志
        LogModule::recordLog(clientIPStr, LOG_UPLOAD, filename, "SUCCESS", totalBytes);
    }

    void deleteFile(const string& filename) {
        string filepath = currentDirectory + "\\" + filename;
        wstring wFilePath = stringToWstring(filepath);
        if (DeleteFileW(wFilePath.c_str())) {
            sendResponse("250 File deleted successfully");
        }
        else {
            sendResponse("550 Could not delete file");
        }
    }

    void makeDirectory(const string& dirname) {
        string dirpath = currentDirectory + "\\" + dirname;
        wstring wDirPath = stringToWstring(dirpath);
        if (CreateDirectoryW(wDirPath.c_str(), NULL)) {
            sendResponse("257 Directory created");
        }
        else {
            sendResponse("550 Could not create directory");
        }
    }

    void removeDirectory(const string& dirname) {
        string dirpath = currentDirectory + "\\" + dirname;
        wstring wDirPath = stringToWstring(dirpath);
        if (RemoveDirectoryW(wDirPath.c_str())) {
            sendResponse("250 Directory removed");
        }
        else {
            sendResponse("550 Could not remove directory");
        }
    }
};

// 线程函数
unsigned __stdcall clientThread(void* param) {
    SOCKET clientSocket = (SOCKET)param;
    sockaddr_in clientAddr;
    int addrLen = sizeof(clientAddr);
    getpeername(clientSocket, (sockaddr*)&clientAddr, &addrLen);

    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIP, INET_ADDRSTRLEN);
    cout << "客户端连接: " << clientIP << ":" << ntohs(clientAddr.sin_port) << endl;

    try {
        FtpSession session(clientSocket, clientAddr);
        session.process();
    }
    catch (const exception& e) {
        cout << "客户端会话结束: " << e.what() << endl;
    }

    cout << "客户端断开: " << clientIP << ":" << ntohs(clientAddr.sin_port) << endl;
    return 0;
}

int main() {
    SetConsoleOutputCP(936);
    // 【新增】初始化全局配置
    ConfigModule::initConfig();

    // 初始化Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cout << "WSAStartup失败" << endl;
        return 1;
    }

    // 创建监听套接字
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == INVALID_SOCKET) {
        cout << "创建套接字失败" << endl;
        WSACleanup();
        return 1;
    }

    // 允许地址重用
    int opt = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    // 绑定地址
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    if (inet_pton(AF_INET, g_ServerConfig.listenIP.c_str(), &serverAddr.sin_addr) <= 0) {
        cout << "Error: Invalid configured IP address: " << g_ServerConfig.listenIP << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    serverAddr.sin_port = htons(g_ServerConfig.listenPort); // 也可以使用配置的端口

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "绑定失败，错误码：" << WSAGetLastError() << endl;
        cout << "Hint: 如果刚修改过 IP，请确保该 IP 属于本机网卡，或者重启服务器。" << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    // 开始监听
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        cout << "监听失败" << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    cout << "========================================" << endl;
    cout << "FTP服务器启动在端口 " << FTP_PORT << endl;
    cout << "本机IP地址: " << endl;

    // 使用getaddrinfo替代gethostbyname
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    struct addrinfo hints, * res, * p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) == 0) {
        for (p = res; p != NULL; p = p->ai_next) {
            struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr->sin_addr), ipStr, INET_ADDRSTRLEN);
            cout << "  " << ipStr << endl;
        }
        freeaddrinfo(res);
    }

    cout << "等待客户端连接..." << endl;
    cout << "========================================" << endl;

    // 【修改后】启动后台线程处理菜单
   // 使用 lambda 表达式调用新模块的函数
    thread menuThread([]() {
        while (true) {
            MenuModule::displayMenu();          // 1. 显示菜单
            MenuOption choice = MenuModule::getUserChoice(); // 2. 获取输入
            MenuModule::handleChoice(choice);   // 3. 处理逻辑

            // 如果选择了退出，handleChoice 内部已经 exit(0)，不会回到这里
            // 如果查看了日志，函数返回后循环会继续，重新显示菜单
        }
        });
    menuThread.detach(); // 分离线程，让其在后台运行

    // 接受客户端连接
    while (true) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            cout << "接受连接失败，错误码: " << WSAGetLastError() << endl;
            continue;
        }

        // 创建线程处理客户端
        unsigned threadId;
        HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, clientThread,
            (void*)clientSocket, 0, &threadId);
        if (thread) {
            CloseHandle(thread); // 分离线程，让它独立运行
        }
        else {
            cout << "创建线程失败" << endl;
            closesocket(clientSocket);
        }
    }

    // 清理
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}