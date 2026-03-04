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
#include <thread>
#include "menu.h"
#include "config_module.h"
#include "db_log_module.h"

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
    string clientIPStr;
    bool authenticated;
    sockaddr_in clientAddr;

    string toUpper(string str) {
        transform(str.begin(), str.end(), str.begin(), ::toupper);
        return str;
    }

    string getFileList(const string& path) {
        string result;
        WIN32_FIND_DATAW findData;
        wstring searchPath = stringToWstring(path + "\\*");
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 ||
                    wcscmp(findData.cFileName, L"..") == 0) {
                    continue;
                }

                string fileName = wstringToString(findData.cFileName);
                string permissions = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
                    "drwxr-xr-x" : "-rw-r--r--";

                LARGE_INTEGER fileSize;
                fileSize.HighPart = findData.nFileSizeHigh;
                fileSize.LowPart = findData.nFileSizeLow;

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
        authenticated = true;

        char ipBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipBuf, INET_ADDRSTRLEN);
        clientIPStr = string(ipBuf);

        char currentPath[BUFFER_SIZE];
        _getcwd(currentPath, BUFFER_SIZE);
        rootDirectory = string(currentPath) + "\\FtpRoot";
        currentDirectory = rootDirectory;

        if (_mkdir(rootDirectory.c_str()) == 0) {
            cout << "Created root directory: " << rootDirectory << endl;
        }

        string publicKeyDir = rootDirectory + "\\PublicKey";
        string uploadDir = rootDirectory + "\\UpLoad";

        if (_mkdir(publicKeyDir.c_str()) == 0) {
            cout << "Created subdirectory: PublicKey" << endl;
        }
        if (_mkdir(uploadDir.c_str()) == 0) {
            cout << "Created subdirectory: UpLoad" << endl;
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
        if (bytesReceived <= 0) return "";
        string command(buffer);
        size_t pos = command.find("\r\n");
        if (pos != string::npos) command = command.substr(0, pos);
        return command;
    }

    void process() {
        sendResponse("220 Simple FTP Server Ready");
        while (true) {
            string commandLine = receiveCommand();
            if (commandLine.empty()) break;
            cout << "收到命令: " << commandLine << endl;
            processCommand(commandLine);
        }
        closesocket(controlSocket);
        if (dataSocket != INVALID_SOCKET) closesocket(dataSocket);
    }

    void processCommand(const string& commandLine) {
        // 解析命令和参数
        size_t spacePos = commandLine.find(' ');
        string cmd = (spacePos != string::npos) ? commandLine.substr(0, spacePos) : commandLine;
        string param = (spacePos != string::npos) ? commandLine.substr(spacePos + 1) : "";

        // 转换为大写用于命令匹配
        string upperCmd = cmd;
        transform(upperCmd.begin(), upperCmd.end(), upperCmd.begin(), ::toupper);

        cout << "收到命令: " << commandLine << endl;
        cout << "命令: " << upperCmd << ", 参数: " << param << endl;

        // 处理用户命令
        if (upperCmd == "USER") {
            sendResponse("331 User name okay, need password");
        }
        // 处理密码命令
        else if (upperCmd == "PASS") {
            sendResponse("230 User logged in");
        }
        // 处理当前目录命令
        else if (upperCmd == "PWD" || upperCmd == "XPWD") {
            string relativePath = getRelativePath(currentDirectory);
            sendResponse("257 \"" + relativePath + "\" is current directory");
        }
        // 处理切换目录命令
        else if (upperCmd == "CWD") {
            changeDirectory(param);
        }
        // 处理返回上级目录命令
        else if (upperCmd == "CDUP") {
            changeDirectory("..");
        }
        // 处理 ls 命令（Windows客户端常用）
        else if (upperCmd == "LS") {
            cout << "处理 LS 命令，参数: " << param << endl;

            // 检查参数
            if (param.empty()) {
                // ls 不带参数 - 显示详细信息
                cout << "LS 无参数，显示详细信息" << endl;
                listDirectory("", false);
            }
            else if (param == "-l") {
                // ls -l - 显示详细信息（长格式）
                cout << "LS -l 参数，显示详细信息" << endl;
                listDirectory("", false);
            }
            else if (param == "-1") {
                // ls -1 - 只显示文件名
                cout << "LS -1 参数，只显示文件名" << endl;
                listDirectory("", true);
            }
            else {
                // 带路径的 ls，如 "ls /folder"
                cout << "LS 带路径: " << param << endl;

                // 检查路径前是否有选项
                if (param[0] == '-') {
                    size_t spacePos2 = param.find(' ');
                    if (spacePos2 != string::npos) {
                        string option = param.substr(0, spacePos2);
                        string path = param.substr(spacePos2 + 1);

                        if (option == "-l") {
                            listDirectory(path, false);
                        }
                        else if (option == "-1") {
                            listDirectory(path, true);
                        }
                        else {
                            listDirectory(path, false);
                        }
                    }
                    else {
                        listDirectory("", false);
                    }
                }
                else {
                    listDirectory(param, false);
                }
            }
        }
        // 处理 dir 命令（Windows客户端常用）- 始终显示详细信息
        else if (upperCmd == "DIR") {
            cout << "处理 DIR 命令" << endl;
            listDirectory(param, false);
        }
        // 处理 LIST 命令（标准FTP命令）
        else if (upperCmd == "LIST") {
            cout << "处理 LIST 命令" << endl;
            listDirectory(param, false);
        }
        // 处理 NLST 命令（标准FTP命令）- 只显示文件名
        else if (upperCmd == "NLST") {
            cout << "处理 NLST 命令" << endl;
            listDirectory(param, true);
        }
        // 处理 PORT 命令
        else if (upperCmd == "PORT") {
            setPort(param);
        }
        // 处理被动模式（暂未实现）
        else if (upperCmd == "PASV") {
            sendResponse("502 Passive mode not implemented");
        }
        // 处理下载文件命令
        else if (upperCmd == "RETR" || upperCmd == "GET") {
            downloadFile(param);
        }
        // 处理上传文件命令
        else if (upperCmd == "STOR" || upperCmd == "PUT") {
            uploadFile(param);
        }
        // 处理删除文件命令
        else if (upperCmd == "DELE" || upperCmd == "DELETE") {
            deleteFile(param);
        }
        // 处理创建目录命令
        else if (upperCmd == "MKD" || upperCmd == "MKDIR") {
            makeDirectory(param);
        }
        // 处理删除目录命令
        else if (upperCmd == "RMD" || upperCmd == "RMDIR") {
            removeDirectory(param);
        }
        // 处理重命名命令（源文件）
        else if (upperCmd == "RNFR") {
            sendResponse("350 Ready for RNTO");
            // 简化实现，实际需要保存文件名
        }
        // 处理重命名命令（目标文件）
        else if (upperCmd == "RNTO") {
            sendResponse("250 Rename successful");
        }
        // 处理传输类型命令
        else if (upperCmd == "TYPE") {
            if (param == "I" || param == "A") {
                sendResponse("200 Type set to " + param);
            }
            else {
                sendResponse("504 Command not implemented for that parameter");
            }
        }
        // 处理系统类型命令
        else if (upperCmd == "SYST") {
            sendResponse("215 Windows_NT");
        }
        // 处理特性命令
        else if (upperCmd == "FEAT") {
            sendResponse("211-Features:\r\n211 End");
        }
        // 处理选项命令
        else if (upperCmd == "OPTS") {
            if (param.find("UTF8") != string::npos) {
                sendResponse("200 UTF8 mode enabled");
            }
            else {
                sendResponse("200 OK");
            }
        }
        // 处理空操作命令
        else if (upperCmd == "NOOP") {
            sendResponse("200 NOOP command successful");
        }
        // 处理退出命令
        else if (upperCmd == "QUIT" || upperCmd == "BYE" || upperCmd == "EXIT") {
            sendResponse("221 Goodbye");
            throw exception("Client disconnected");
        }
        // 未知命令
        else {
            cout << "未知命令: " << upperCmd << endl;
            sendResponse("502 Command not implemented: " + upperCmd);
        }
    }

    string getRelativePath(const string& path) {
        if (path.find(rootDirectory) == 0) {
            string relPath = path.substr(rootDirectory.length());
            for (char& c : relPath) if (c == '\\') c = '/';
            if (relPath.empty() || relPath[0] != '/') relPath = "/" + relPath;
            return relPath;
        }
        return "/";
    }

    void changeDirectory(const string& path) {
        string newPath;
        if (path.empty()) newPath = rootDirectory;
        else if (path == "/") newPath = rootDirectory;
        else if (path == "..") {
            size_t pos = currentDirectory.find_last_of('\\');
            if (pos != string::npos && currentDirectory.length() > rootDirectory.length())
                newPath = currentDirectory.substr(0, pos);
            else newPath = rootDirectory;
        }
        else if (path[0] == '/') {
            newPath = rootDirectory + path;
            for (char& c : newPath) if (c == '/') c = '\\';
        }
        else newPath = currentDirectory + "\\" + path;

        struct stat info;
        if (stat(newPath.c_str(), &info) == 0 && (info.st_mode & S_IFDIR)) {
            currentDirectory = newPath;
            sendResponse("250 Directory changed to " + getRelativePath(currentDirectory));
        }
        else sendResponse("550 Directory not found: " + path);
    }

    void setPort(const string& param) {
        vector<string> parts;
        stringstream ss(param);
        string part;
        while (getline(ss, part, ',')) parts.push_back(part);

        if (parts.size() == 6) {
            string ip = parts[0] + "." + parts[1] + "." + parts[2] + "." + parts[3];
            int port = (stoi(parts[4]) << 8) + stoi(parts[5]);
            cout << "PORT命令: IP=" << ip << ", Port=" << port << endl;

            if (dataSocket != INVALID_SOCKET) closesocket(dataSocket);
            dataSocket = socket(AF_INET, SOCK_STREAM, 0);
            if (dataSocket == INVALID_SOCKET) {
                sendResponse("425 Can't create data socket");
                return;
            }

            sockaddr_in dataAddr;
            dataAddr.sin_family = AF_INET;
            dataAddr.sin_port = htons(port);
            if (inet_pton(AF_INET, ip.c_str(), &dataAddr.sin_addr) <= 0) {
                sendResponse("425 Invalid IP address");
                closesocket(dataSocket);
                dataSocket = INVALID_SOCKET;
                return;
            }

            if (connect(dataSocket, (sockaddr*)&dataAddr, sizeof(dataAddr)) == 0)
                sendResponse("200 Port command successful");
            else {
                sendResponse("425 Can't open data connection");
                closesocket(dataSocket);
                dataSocket = INVALID_SOCKET;
            }
        }
        else sendResponse("501 Syntax error in parameters");
    }



    // 只获取文件名的列表（NLST模式）
    string getNameOnlyList(const string& path) {
        string result;
        WIN32_FIND_DATAW findData;
        wstring searchPath = stringToWstring(path + "\\*");
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 ||
                    wcscmp(findData.cFileName, L"..") == 0) {
                    continue;
                }

                string fileName = wstringToString(findData.cFileName);
                result += fileName + "\r\n";

            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
        return result;
    }

    void listDirectory(const string& path, bool nameOnly = false) {
        if (dataSocket == INVALID_SOCKET) {
            sendResponse("425 No data connection");
            return;
        }

        // 根据模式发送不同的响应消息
        if (nameOnly) {
            sendResponse("150 Opening ASCII mode data connection for file list");
        }
        else {
            sendResponse("150 Opening data connection for directory list");
        }

        // 确定目标路径
        string targetPath = currentDirectory;
        if (!path.empty() && path != "-a" && path != "-l" && path != "-1") {
            if (path[0] == '/') {
                targetPath = rootDirectory + path;
                for (char& c : targetPath) if (c == '/') c = '\\';
            }
            else {
                targetPath = currentDirectory + "\\" + path;
            }
        }

        cout << "列出目录: " << targetPath << ", 模式: " << (nameOnly ? "仅文件名" : "详细信息") << endl;

        string fileList;
        if (nameOnly) {
            // NLST 模式：只返回文件名
            fileList = getNameOnlyList(targetPath);
        }
        else {
            // LIST 模式：返回详细信息
            fileList = getFileList(targetPath);
        }

        // 发送数据
        int totalSent = 0;
        int bytesToSend = fileList.length();
        while (totalSent < bytesToSend) {
            int sent = send(dataSocket, fileList.c_str() + totalSent,
                bytesToSend - totalSent, 0);
            if (sent <= 0) {
                cout << "发送数据失败" << endl;
                break;
            }
            totalSent += sent;
        }

        cout << "已发送 " << totalSent << " 字节的数据" << endl;

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

        streampos fileSize = file.tellg();
        file.seekg(0, ios::beg);
        sendResponse("150 Opening data connection for file download (" + to_string(fileSize) + " bytes)");

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
        if (file.gcount() > 0) send(dataSocket, buffer, file.gcount(), 0);

        file.close();
        closesocket(dataSocket);
        dataSocket = INVALID_SOCKET;
        sendResponse("226 File send OK");
        DbLogModule::recordLog(clientIPStr, DB_LOG_DOWNLOAD, filename, "SUCCESS", (long)fileSize);
    }


    // 不区分大小写的字符串查找函数
    bool caseInsensitiveFind(const string& haystack, const string& needle) {
        if (needle.empty()) return false;

        string haystackLower = haystack;
        string needleLower = needle;

        // 转换为小写
        transform(haystackLower.begin(), haystackLower.end(), haystackLower.begin(), ::tolower);
        transform(needleLower.begin(), needleLower.end(), needleLower.begin(), ::tolower);

        return haystackLower.find(needleLower) != string::npos;
    }


    // 在 main 函数中，初始化数据库后调用
    int main() {
        SetConsoleOutputCP(936);

        // 初始化数据库日志模块
        if (!DbLogModule::initialize("ftp_server.db")) {
            cout << "警告: 数据库日志模块初始化失败" << endl;
            return 1;
        }
        else {
            cout << "数据库日志模块初始化成功" << endl;


            DbLogModule::recordLog("SYSTEM", DB_LOG_SERVER_START, "FTP Server", "STARTED", 0);
        }

        // ... 其余代码 ...
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

        // 只保留日志记录，移除两个表的插入代码
        DbLogModule::recordLog(clientIPStr, DB_LOG_UPLOAD, filename, "SUCCESS", totalBytes);

        // 调试输出当前目录（可选保留）
        cout << "当前目录: " << currentDirectory << endl;
        cout << "上传文件名: " << filename << endl;
        cout << "文件大小: " << totalBytes << " bytes" << endl;
    }

    void deleteFile(const string& filename) {
        string filepath = currentDirectory + "\\" + filename;
        wstring wFilePath = stringToWstring(filepath);
        if (DeleteFileW(wFilePath.c_str())) sendResponse("250 File deleted successfully");
        else sendResponse("550 Could not delete file");
    }

    void makeDirectory(const string& dirname) {
        string dirpath = currentDirectory + "\\" + dirname;
        wstring wDirPath = stringToWstring(dirpath);
        if (CreateDirectoryW(wDirPath.c_str(), NULL)) sendResponse("257 Directory created");
        else sendResponse("550 Could not create directory");
    }

    void removeDirectory(const string& dirname) {
        string dirpath = currentDirectory + "\\" + dirname;
        wstring wDirPath = stringToWstring(dirpath);
        if (RemoveDirectoryW(wDirPath.c_str())) sendResponse("250 Directory removed");
        else sendResponse("550 Could not remove directory");
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

    // 初始化数据库日志模块
    if (!DbLogModule::initialize("ftp_server.db")) {
        cout << "警告: 数据库日志模块初始化失败" << endl;
        return 1;
    }
    else {
        cout << "数据库日志模块初始化成功" << endl;


        DbLogModule::recordLog("SYSTEM", DB_LOG_SERVER_START, "FTP Server", "STARTED", 0);
    }
    // 初始化全局配置
    ConfigModule::initConfig();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cout << "WSAStartup失败" << endl;
        return 1;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == INVALID_SOCKET) {
        cout << "创建套接字失败" << endl;
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    if (inet_pton(AF_INET, g_ServerConfig.listenIP.c_str(), &serverAddr.sin_addr) <= 0) {
        cout << "Error: Invalid configured IP address: " << g_ServerConfig.listenIP << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }
    serverAddr.sin_port = htons(g_ServerConfig.listenPort);

    if (::bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "绑定失败，错误码：" << WSAGetLastError() << endl;
        cout << "Hint: 如果刚修改过 IP，请确保该 IP 属于本机网卡，或者重启服务器。" << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        cout << "监听失败" << endl;
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    cout << "========================================" << endl;
    cout << "FTP服务器启动在端口 " << FTP_PORT << endl;
    cout << "本机IP地址: " << endl;

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    struct addrinfo hints, * res, * p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
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

    thread menuThread([]() {
        while (true) {
            MenuModule::displayMenu();
            MenuOption choice = MenuModule::getUserChoice();
            MenuModule::handleChoice(choice);
        }
        });
    menuThread.detach();

    while (true) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            cout << "接受连接失败，错误码: " << WSAGetLastError() << endl;
            continue;
        }

        unsigned threadId;
        HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, clientThread, (void*)clientSocket, 0, &threadId);
        if (thread) CloseHandle(thread);
        else {
            cout << "创建线程失败" << endl;
            closesocket(clientSocket);
        }
    }

    DbLogModule::shutdown();
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}