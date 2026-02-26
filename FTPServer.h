#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <locale.h>
#include <tchar.h>
#include <time.h>
#include <vector>
#include <string>
#include <map>
#include <wincrypt.h>

#include "sqlite3.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")
#pragma comment(lib, "crypt32.lib")

#define DEFAULT_PORT 21
#define BUFFER_SIZE 4096
#define MAX_CLIENTS 1000
#define DATA_PORT_START 50000
#define DATA_PORT_END 50010
#define SESSION_TIMEOUT 300
#define TOKEN_LENGTH 32
#define MAX_PASSWORD_ATTEMPTS 3
#define ACCOUNT_LOCK_TIME 300

// 客户端状态
enum ClientState {
    STATE_NOT_LOGGED_IN,
    STATE_LOGGED_IN,
    STATE_TRANSFERRING,
    STATE_REGISTERING
};

// 传输模式
enum TransferMode {
    MODE_NONE,
    MODE_UPLOAD,
    MODE_DOWNLOAD
};

// 用户信息结构
struct UserInfo {
    int userId;
    char username[64];
    char passwordHash[128];
    char salt[32];
    char email[128];
    char homeDir[MAX_PATH];
    time_t registerTime;
    time_t lastLoginTime;
    int loginCount;
    int failedAttempts;
    time_t lockUntil;
    BOOL isActive;
    BOOL isAdmin;
};

// 会话信息结构
struct SessionInfo {
    char token[TOKEN_LENGTH + 1];
    int clientId;
    int userId;
    char username[64];
    time_t createTime;
    time_t lastActiveTime;
    BOOL isValid;
};

// 客户端上下文结构
struct ClientContext {
    OVERLAPPED overlapped;
    SOCKET socket;
    SOCKET dataSocket;
    WSABUF wsabuf;
    char buffer[BUFFER_SIZE];
    char currentDir[MAX_PATH];
    char username[256];
    char sessionToken[TOKEN_LENGTH + 1];
    ClientState state;
    TransferMode transferMode;
    HANDLE fileHandle;
    DWORD bytesSent;
    DWORD bytesToSend;
    DWORD bytesReceived;
    DWORD bytesToReceive;
    DWORD bytesTransferred;
    BOOL isDataConnected;
    int dataPort;
    sockaddr_in dataAddr;
    int clientId;
    int userId;
    time_t lastActivityTime;
    BOOL isAuthenticated;
    int failedAttempts;
};

// 完成端口数据结构
struct CompletionPortData {
    ULONG_PTR completionKey;
    DWORD bytesTransferred;
    LPOVERLAPPED overlapped;
};

class CFTPServer {
private:
    SOCKET m_listenSocket;
    HANDLE m_completionPort;
    HANDLE m_workerThreads[MAX_CLIENTS];
    HANDLE m_sessionMonitorThread;
    int m_threadCount;
    ClientContext* m_clients[MAX_CLIENTS];
    int m_clientCount;
    CRITICAL_SECTION m_cs;
    CRITICAL_SECTION m_userCs;
    CRITICAL_SECTION m_sessionCs;

    sqlite3* m_db;
    char m_dbPath[MAX_PATH];

    std::map<std::string, SessionInfo> m_sessions;
    std::map<int, std::string> m_clientSessionMap;

public:
    CFTPServer();
    ~CFTPServer();

    BOOL Initialize(int port = DEFAULT_PORT);
    void Run();
    void Shutdown();

private:
    static unsigned __stdcall WorkerThread(LPVOID lpParam);
    static unsigned __stdcall AcceptThread(LPVOID lpParam);
    static unsigned __stdcall SessionMonitorThread(LPVOID lpParam);

    void ProcessIO(ClientContext* context, DWORD bytesTransferred);
    void HandleCommand(ClientContext* context);
    void SendResponse(ClientContext* context, const char* response);
    void ProcessDataConnection(ClientContext* context);

    // 用户管理命令
    void HandleUSER(ClientContext* context, const char* param);
    void HandlePASS(ClientContext* context, const char* param);
    void HandleREGISTER(ClientContext* context, const char* param);
    void HandleLOGOUT(ClientContext* context);
    void HandleWHOAMI(ClientContext* context);
    void HandleUSERS(ClientContext* context);
    void HandleCHPASS(ClientContext* context, const char* param);

    // FTP命令
    void HandleSYST(ClientContext* context);
    void HandleFEAT(ClientContext* context);
    void HandlePWD(ClientContext* context);
    void HandleCWD(ClientContext* context, const char* param);
    void HandleLIST(ClientContext* context);
    void HandleRETR(ClientContext* context, const char* param);
    void HandleSTOR(ClientContext* context, const char* param);
    void HandleQUIT(ClientContext* context);
    void HandlePORT(ClientContext* context, const char* param);
    void HandlePASV(ClientContext* context);
    void HandleTYPE(ClientContext* context, const char* param);

    // 数据库操作
    BOOL InitDatabase();
    BOOL CreateTables();
    BOOL AddUser(const char* username, const char* password, const char* email);
    BOOL AuthenticateUser(const char* username, const char* password, int* userId);
    BOOL UpdateLoginInfo(int userId, BOOL success);
    BOOL IsUserLocked(const char* username);
    BOOL GetUserInfo(const char* username, UserInfo* userInfo);
    BOOL UpdatePassword(int userId, const char* newPassword);

    // 密码处理
    void GenerateSalt(char* salt, int length);
    void HashPassword(const char* password, const char* salt, char* hash);
    BOOL VerifyPassword(const char* inputPassword, const char* storedHash, const char* salt);

    // 会话管理
    BOOL GenerateSessionToken(ClientContext* context);
    BOOL ValidateSession(ClientContext* context);
    void InvalidateSession(ClientContext* context);
    void UpdateSessionActivity(ClientContext* context);
    void CleanupExpiredSessions();

    // 辅助函数
    void GenerateToken(char* token, int length);
    void CreateUserDirectory(const char* username);
    void SetupDataConnection(ClientContext* context);
    void CloseDataConnection(ClientContext* context);
    void StartFileTransfer(ClientContext* context, const char* filename, BOOL isDownload);
    void ContinueFileTransfer(ClientContext* context, DWORD bytesTransferred);

    ClientContext* CreateClientContext(SOCKET clientSocket);
    void RemoveClientContext(ClientContext* context);
    ClientContext* FindClientBySocket(SOCKET socket);
    ClientContext* FindClientByUsername(const char* username);

    void GetUserHomeDirectory(char* path, const char* username);
    BOOL IsPathSafe(const char* basePath, const char* requestedPath);
};

#endif