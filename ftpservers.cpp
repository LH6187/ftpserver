#include "FTPServer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <random>

// 辅助函数：将多字节字符串转换为宽字符串
std::wstring MBToWide(const char* mbStr) {
    if (mbStr == nullptr) return L"";
    int len = MultiByteToWideChar(CP_ACP, 0, mbStr, -1, NULL, 0);
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_ACP, 0, mbStr, -1, &wstr[0], len);
    return wstr;
}

// 辅助函数：将宽字符串转换为多字节字符串
std::string WideToMB(const wchar_t* wStr) {
    if (wStr == nullptr) return "";
    int len = WideCharToMultiByte(CP_ACP, 0, wStr, -1, NULL, 0, NULL, NULL);
    std::string str(len, 0);
    WideCharToMultiByte(CP_ACP, 0, wStr, -1, &str[0], len, NULL, NULL);
    return str;
}

CFTPServer::CFTPServer() {
    m_listenSocket = INVALID_SOCKET;
    m_completionPort = NULL;
    m_threadCount = 0;
    m_clientCount = 0;
    m_sessionMonitorThread = NULL;
    m_db = NULL;

    ZeroMemory(m_clients, sizeof(m_clients));
    ZeroMemory(m_dbPath, sizeof(m_dbPath));

    InitializeCriticalSection(&m_cs);
    InitializeCriticalSection(&m_userCs);
    InitializeCriticalSection(&m_sessionCs);

    GetCurrentDirectoryA(MAX_PATH, m_dbPath);
    strcat_s(m_dbPath, "\\ftp_users.db");
}

CFTPServer::~CFTPServer() {
    Shutdown();
    DeleteCriticalSection(&m_cs);
    DeleteCriticalSection(&m_userCs);
    DeleteCriticalSection(&m_sessionCs);
}

// ==================== 密码处理 ====================

void CFTPServer::GenerateSalt(char* salt, int length) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*()";
    const int charsetSize = sizeof(charset) - 1;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, charsetSize - 1);

    for (int i = 0; i < length - 1; i++) {
        salt[i] = charset[dis(gen)];
    }
    salt[length - 1] = '\0';
}

void CFTPServer::HashPassword(const char* password, const char* salt, char* hash) {
    char combined[256];
    sprintf_s(combined, sizeof(combined), "%s%s", password, salt);

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE pbHash[32];
    DWORD dwHashLen = sizeof(pbHash);

    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
            CryptHashData(hHash, (BYTE*)combined, (DWORD)strlen(combined), 0);
            CryptGetHashParam(hHash, HP_HASHVAL, pbHash, &dwHashLen, 0);

            for (DWORD i = 0; i < dwHashLen; i++) {
                sprintf_s(hash + (i * 2), 3, "%02x", pbHash[i]);
            }
            hash[dwHashLen * 2] = '\0';

            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    else {
        unsigned long h = 5381;
        for (size_t i = 0; i < strlen(combined); i++) {
            h = ((h << 5) + h) + combined[i];
        }
        sprintf_s(hash, 128, "%016llx", h);
    }
}

BOOL CFTPServer::VerifyPassword(const char* inputPassword, const char* storedHash, const char* salt) {
    char computedHash[128];
    HashPassword(inputPassword, salt, computedHash);
    return strcmp(computedHash, storedHash) == 0;
}

void CFTPServer::GenerateToken(char* token, int length) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const int charsetSize = sizeof(charset) - 1;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, charsetSize - 1);

    for (int i = 0; i < length; i++) {
        token[i] = charset[dis(gen)];
    }
    token[length] = '\0';
}

// ==================== 数据库操作 ====================

BOOL CFTPServer::InitDatabase() {
    int rc = sqlite3_open(m_dbPath, &m_db);
    if (rc != SQLITE_OK) {
        printf("Cannot open database: %s\n", sqlite3_errmsg(m_db));
        return FALSE;
    }

    return CreateTables();
}

BOOL CFTPServer::CreateTables() {
    const char* sql_users =
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "username TEXT UNIQUE NOT NULL,"
        "password_hash TEXT NOT NULL,"
        "salt TEXT NOT NULL,"
        "email TEXT,"
        "home_dir TEXT,"
        "register_time INTEGER,"
        "last_login_time INTEGER,"
        "login_count INTEGER DEFAULT 0,"
        "failed_attempts INTEGER DEFAULT 0,"
        "lock_until INTEGER DEFAULT 0,"
        "is_active INTEGER DEFAULT 1,"
        "is_admin INTEGER DEFAULT 0"
        ");";

    const char* sql_sessions =
        "CREATE TABLE IF NOT EXISTS sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER,"
        "token TEXT UNIQUE,"
        "client_ip TEXT,"
        "create_time INTEGER,"
        "last_active INTEGER,"
        "FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";

    const char* sql_logs =
        "CREATE TABLE IF NOT EXISTS login_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "user_id INTEGER,"
        "username TEXT,"
        "login_time INTEGER,"
        "success INTEGER,"
        "client_ip TEXT,"
        "FOREIGN KEY(user_id) REFERENCES users(id)"
        ");";

    char* errMsg = NULL;

    int rc = sqlite3_exec(m_db, sql_users, NULL, 0, &errMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error (users): %s\n", errMsg);
        sqlite3_free(errMsg);
        return FALSE;
    }

    rc = sqlite3_exec(m_db, sql_sessions, NULL, 0, &errMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error (sessions): %s\n", errMsg);
        sqlite3_free(errMsg);
        return FALSE;
    }

    rc = sqlite3_exec(m_db, sql_logs, NULL, 0, &errMsg);
    if (rc != SQLITE_OK) {
        printf("SQL error (logs): %s\n", errMsg);
        sqlite3_free(errMsg);
        return FALSE;
    }

    // 检查是否有默认管理员账户
    const char* check_admin = "SELECT COUNT(*) FROM users WHERE username = 'admin'";
    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(m_db, check_admin, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            if (count == 0) {
                char salt[32];
                GenerateSalt(salt, sizeof(salt));
                char hash[128];
                HashPassword("admin123", salt, hash);

                const char* insert_admin =
                    "INSERT INTO users (username, password_hash, salt, email, home_dir, register_time, is_admin) "
                    "VALUES ('admin', ?, ?, 'admin@localhost', 'C:\\FTPServer\\users\\admin', ?, 1)";

                sqlite3_stmt* insert_stmt;
                if (sqlite3_prepare_v2(m_db, insert_admin, -1, &insert_stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(insert_stmt, 1, hash, -1, SQLITE_STATIC);
                    sqlite3_bind_text(insert_stmt, 2, salt, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(insert_stmt, 3, (sqlite3_int64)time(NULL));

                    if (sqlite3_step(insert_stmt) == SQLITE_DONE) {
                        printf("Default admin account created\n");

                        CreateDirectoryA("C:\\FTPServer", NULL);
                        CreateDirectoryA("C:\\FTPServer\\users", NULL);
                        CreateDirectoryA("C:\\FTPServer\\users\\admin", NULL);
                    }
                    sqlite3_finalize(insert_stmt);
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    printf("Database initialized successfully\n");
    return TRUE;
}

BOOL CFTPServer::AddUser(const char* username, const char* password, const char* email) {
    EnterCriticalSection(&m_userCs);

    const char* check_sql = "SELECT id FROM users WHERE username = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, check_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_finalize(stmt);
            LeaveCriticalSection(&m_userCs);
            return FALSE;
        }
        sqlite3_finalize(stmt);
    }

    char salt[32];
    GenerateSalt(salt, sizeof(salt));
    char hash[128];
    HashPassword(password, salt, hash);

    const char* insert_sql =
        "INSERT INTO users (username, password_hash, salt, email, home_dir, register_time, login_count) "
        "VALUES (?, ?, ?, ?, ?, ?, 0)";

    rc = sqlite3_prepare_v2(m_db, insert_sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        char homeDir[MAX_PATH];
        sprintf_s(homeDir, sizeof(homeDir), "C:\\FTPServer\\users\\%s", username);

        sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, salt, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, email, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, homeDir, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)time(NULL));

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            LeaveCriticalSection(&m_userCs);
            return TRUE;
        }
    }

    LeaveCriticalSection(&m_userCs);
    return FALSE;
}

BOOL CFTPServer::AuthenticateUser(const char* username, const char* password, int* userId) {
    EnterCriticalSection(&m_userCs);

    if (IsUserLocked(username)) {
        LeaveCriticalSection(&m_userCs);
        return FALSE;
    }

    const char* sql =
        "SELECT id, password_hash, salt, failed_attempts, lock_until, is_active "
        "FROM users WHERE username = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LeaveCriticalSection(&m_userCs);
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    BOOL authenticated = FALSE;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char* storedHash = (const char*)sqlite3_column_text(stmt, 1);
        const char* salt = (const char*)sqlite3_column_text(stmt, 2);
        int failedAttempts = sqlite3_column_int(stmt, 3);
        int isActive = sqlite3_column_int(stmt, 5);

        if (isActive && VerifyPassword(password, storedHash, salt)) {
            authenticated = TRUE;
            if (userId) *userId = id;

            const char* reset_sql = "UPDATE users SET failed_attempts = 0, last_login_time = ? WHERE id = ?";
            sqlite3_stmt* reset_stmt;
            if (sqlite3_prepare_v2(m_db, reset_sql, -1, &reset_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(reset_stmt, 1, (sqlite3_int64)time(NULL));
                sqlite3_bind_int(reset_stmt, 2, id);
                sqlite3_step(reset_stmt);
                sqlite3_finalize(reset_stmt);
            }
        }
        else {
            failedAttempts++;
            const char* update_sql = "UPDATE users SET failed_attempts = ? WHERE id = ?";
            sqlite3_stmt* update_stmt;
            if (sqlite3_prepare_v2(m_db, update_sql, -1, &update_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int(update_stmt, 1, failedAttempts);
                sqlite3_bind_int(update_stmt, 2, id);
                sqlite3_step(update_stmt);
                sqlite3_finalize(update_stmt);
            }

            if (failedAttempts >= MAX_PASSWORD_ATTEMPTS) {
                const char* lock_sql = "UPDATE users SET lock_until = ? WHERE id = ?";
                sqlite3_stmt* lock_stmt;
                if (sqlite3_prepare_v2(m_db, lock_sql, -1, &lock_stmt, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(lock_stmt, 1, (sqlite3_int64)(time(NULL) + ACCOUNT_LOCK_TIME));
                    sqlite3_bind_int(lock_stmt, 2, id);
                    sqlite3_step(lock_stmt);
                    sqlite3_finalize(lock_stmt);
                }
            }
        }
    }

    sqlite3_finalize(stmt);

    const char* log_sql =
        "INSERT INTO login_logs (user_id, username, login_time, success, client_ip) "
        "VALUES (?, ?, ?, ?, ?)";

    sqlite3_stmt* log_stmt;
    if (sqlite3_prepare_v2(m_db, log_sql, -1, &log_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(log_stmt, 1, authenticated ? *userId : -1);
        sqlite3_bind_text(log_stmt, 2, username, -1, SQLITE_STATIC);
        sqlite3_bind_int64(log_stmt, 3, (sqlite3_int64)time(NULL));
        sqlite3_bind_int(log_stmt, 4, authenticated ? 1 : 0);
        sqlite3_bind_text(log_stmt, 5, "127.0.0.1", -1, SQLITE_STATIC);
        sqlite3_step(log_stmt);
        sqlite3_finalize(log_stmt);
    }

    LeaveCriticalSection(&m_userCs);
    return authenticated;
}

BOOL CFTPServer::IsUserLocked(const char* username) {
    const char* sql = "SELECT lock_until FROM users WHERE username = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    BOOL locked = FALSE;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t lockUntil = (time_t)sqlite3_column_int64(stmt, 0);
        if (lockUntil > time(NULL)) {
            locked = TRUE;
        }
    }

    sqlite3_finalize(stmt);
    return locked;
}

BOOL CFTPServer::UpdateLoginInfo(int userId, BOOL success) {
    const char* sql = "UPDATE users SET login_count = login_count + 1 WHERE id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_int(stmt, 1, userId);
    BOOL result = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    return result;
}

BOOL CFTPServer::UpdatePassword(int userId, const char* newPassword) {
    char salt[32];
    GenerateSalt(salt, sizeof(salt));
    char hash[128];
    HashPassword(newPassword, salt, hash);

    const char* sql = "UPDATE users SET password_hash = ?, salt = ? WHERE id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, salt, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, userId);

    BOOL result = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    return result;
}

// ==================== 会话管理 ====================

BOOL CFTPServer::GenerateSessionToken(ClientContext* context) {
    char token[TOKEN_LENGTH + 1];
    GenerateToken(token, TOKEN_LENGTH);
    strcpy_s(context->sessionToken, sizeof(context->sessionToken), token);

    SessionInfo session;
    strcpy_s(session.token, token);
    session.clientId = context->clientId;
    session.userId = context->userId;
    strcpy_s(session.username, context->username);
    session.createTime = time(NULL);
    session.lastActiveTime = time(NULL);
    session.isValid = TRUE;

    EnterCriticalSection(&m_sessionCs);

    for (auto& pair : m_sessions) {
        if (pair.second.userId == context->userId) {
            pair.second.isValid = FALSE;
        }
    }

    m_sessions[token] = session;
    m_clientSessionMap[context->clientId] = token;

    LeaveCriticalSection(&m_sessionCs);

    return TRUE;
}

BOOL CFTPServer::ValidateSession(ClientContext* context) {
    if (!context->isAuthenticated) {
        return FALSE;
    }

    EnterCriticalSection(&m_sessionCs);

    auto it = m_sessions.find(context->sessionToken);
    if (it != m_sessions.end() && it->second.isValid) {
        time_t now = time(NULL);
        if (now - it->second.lastActiveTime > SESSION_TIMEOUT) {
            it->second.isValid = FALSE;
            context->isAuthenticated = FALSE;
            LeaveCriticalSection(&m_sessionCs);
            return FALSE;
        }

        it->second.lastActiveTime = now;
        context->lastActivityTime = now;

        LeaveCriticalSection(&m_sessionCs);
        return TRUE;
    }

    LeaveCriticalSection(&m_sessionCs);
    context->isAuthenticated = FALSE;
    return FALSE;
}

void CFTPServer::InvalidateSession(ClientContext* context) {
    EnterCriticalSection(&m_sessionCs);

    auto it = m_sessions.find(context->sessionToken);
    if (it != m_sessions.end()) {
        it->second.isValid = FALSE;
        m_sessions.erase(it);
    }

    m_clientSessionMap.erase(context->clientId);

    LeaveCriticalSection(&m_sessionCs);
}

void CFTPServer::UpdateSessionActivity(ClientContext* context) {
    EnterCriticalSection(&m_sessionCs);

    auto it = m_sessions.find(context->sessionToken);
    if (it != m_sessions.end() && it->second.isValid) {
        it->second.lastActiveTime = time(NULL);
    }

    LeaveCriticalSection(&m_sessionCs);
}

void CFTPServer::CleanupExpiredSessions() {
    time_t now = time(NULL);

    EnterCriticalSection(&m_sessionCs);

    auto it = m_sessions.begin();
    while (it != m_sessions.end()) {
        if (!it->second.isValid || (now - it->second.lastActiveTime > SESSION_TIMEOUT)) {
            printf("Session expired for user: %s\n", it->second.username);

            ClientContext* client = FindClientByUsername(it->second.username);
            if (client) {
                client->isAuthenticated = FALSE;
                client->state = STATE_NOT_LOGGED_IN;
            }

            m_clientSessionMap.erase(it->second.clientId);
            it = m_sessions.erase(it);
        }
        else {
            ++it;
        }
    }

    LeaveCriticalSection(&m_sessionCs);
}

// ==================== 初始化 ====================

BOOL CFTPServer::Initialize(int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return FALSE;
    }

    if (!InitDatabase()) {
        printf("Failed to initialize database\n");
        WSACleanup();
        return FALSE;
    }

    m_listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (m_listenSocket == INVALID_SOCKET) {
        printf("WSASocket failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(port);

    if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("bind failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        printf("listen failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    m_completionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (m_completionPort == NULL) {
        printf("CreateIoCompletionPort failed\n");
        return FALSE;
    }

    if (CreateIoCompletionPort((HANDLE)m_listenSocket, m_completionPort, 0, 0) == NULL) {
        printf("CreateIoCompletionPort for listen socket failed\n");
        return FALSE;
    }

    printf("FTP Server initialized on port %d\n", port);
    printf("Database path: %s\n", m_dbPath);

    return TRUE;
}

void CFTPServer::Run() {
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    m_threadCount = systemInfo.dwNumberOfProcessors * 2;

    for (int i = 0; i < m_threadCount; i++) {
        m_workerThreads[i] = (HANDLE)_beginthreadex(NULL, 0, WorkerThread, this, 0, NULL);
    }

    m_sessionMonitorThread = (HANDLE)_beginthreadex(NULL, 0, SessionMonitorThread, this, 0, NULL);

    _beginthreadex(NULL, 0, AcceptThread, this, 0, NULL);

    printf("Server is running. Press Enter to shutdown...\n");
    getchar();
}

// ==================== 线程函数 ====================

unsigned __stdcall CFTPServer::SessionMonitorThread(LPVOID lpParam) {
    CFTPServer* server = (CFTPServer*)lpParam;

    while (true) {
        Sleep(10000);
        server->CleanupExpiredSessions();
    }

    return 0;
}

unsigned __stdcall CFTPServer::WorkerThread(LPVOID lpParam) {
    CFTPServer* server = (CFTPServer*)lpParam;
    DWORD bytesTransferred;
    ClientContext* context;
    LPOVERLAPPED overlapped;

    while (true) {
        BOOL result = GetQueuedCompletionStatus(
            server->m_completionPort,
            &bytesTransferred,
            (PULONG_PTR)&context,
            &overlapped,
            INFINITE
        );

        if (!result) {
            if (context != NULL) {
                closesocket(context->socket);
                server->RemoveClientContext(context);
            }
            continue;
        }

        if (bytesTransferred == 0 && context != NULL) {
            closesocket(context->socket);
            server->RemoveClientContext(context);
            continue;
        }

        if (context != NULL) {
            context->bytesTransferred = bytesTransferred;
            context->lastActivityTime = time(NULL);
            server->ProcessIO(context, bytesTransferred);
        }
    }

    return 0;
}

unsigned __stdcall CFTPServer::AcceptThread(LPVOID lpParam) {
    CFTPServer* server = (CFTPServer*)lpParam;

    while (true) {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);

        SOCKET clientSocket = WSAAccept(server->m_listenSocket, (sockaddr*)&clientAddr, &addrLen, NULL, NULL);

        if (clientSocket == INVALID_SOCKET) {
            printf("Accept failed: %d\n", WSAGetLastError());
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        InetNtopA(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

        printf("New client connected from %s:%d\n", ipStr, ntohs(clientAddr.sin_port));

        ClientContext* context = server->CreateClientContext(clientSocket);

        if (context == NULL) {
            closesocket(clientSocket);
            continue;
        }

        if (CreateIoCompletionPort((HANDLE)clientSocket, server->m_completionPort, (ULONG_PTR)context, 0) == NULL) {
            printf("CreateIoCompletionPort for client failed\n");
            server->RemoveClientContext(context);
            closesocket(clientSocket);
            continue;
        }

        char welcome[512];
        sprintf_s(welcome, sizeof(welcome),
            "220 FTP Server ready\r\n"
            "230- Welcome to FTP Server with User Management\r\n"
            "230- Commands:\r\n"
            "230-   REGISTER <username> <password> [email] - Register new account\r\n"
            "230-   USER <username> - Enter username\r\n"
            "230-   PASS <password> - Enter password\r\n"
            "230-   LOGOUT - Logout current user\r\n"
            "230-   WHOAMI - Show current user info\r\n"
            "230-   USERS - List online users\r\n"
            "230-   CHPASS <old> <new> - Change password\r\n"
            "230-\r\n");
        server->SendResponse(context, welcome);

        DWORD flags = 0;
        DWORD recvBytes = 0;
        ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));
        context->wsabuf.len = BUFFER_SIZE - 1;
        context->wsabuf.buf = context->buffer;

        if (WSARecv(context->socket, &context->wsabuf, 1, &recvBytes, &flags, &context->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                printf("WSARecv failed: %d\n", WSAGetLastError());
                server->RemoveClientContext(context);
                closesocket(clientSocket);
            }
        }
    }

    return 0;
}

// ==================== 命令处理 ====================

void CFTPServer::ProcessIO(ClientContext* context, DWORD bytesTransferred) {
    context->lastActivityTime = time(NULL);

    if (context->dataSocket != INVALID_SOCKET && context->isDataConnected) {
        ProcessDataConnection(context);
    }
    else {
        context->buffer[bytesTransferred] = '\0';
        HandleCommand(context);

        DWORD flags = 0;
        DWORD recvBytes = 0;
        ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));
        context->wsabuf.len = BUFFER_SIZE - 1;
        context->wsabuf.buf = context->buffer;

        if (WSARecv(context->socket, &context->wsabuf, 1, &recvBytes, &flags, &context->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                printf("WSARecv failed: %d\n", WSAGetLastError());
                RemoveClientContext(context);
                closesocket(context->socket);
            }
        }
    }
}

void CFTPServer::HandleCommand(ClientContext* context) {
    char command[16] = { 0 };
    char param1[256] = { 0 };
    char param2[256] = { 0 };
    char param3[256] = { 0 };

    int result = sscanf_s(context->buffer, "%15s %255s %255s %255s",
        command, (unsigned)_countof(command),
        param1, (unsigned)_countof(param1),
        param2, (unsigned)_countof(param2),
        param3, (unsigned)_countof(param3));

    if (result < 1) return;

    for (int i = 0; command[i]; i++) {
        command[i] = toupper(command[i]);
    }

    if (strcmp(command, "REGISTER") != 0 &&
        strcmp(command, "USER") != 0 &&
        strcmp(command, "PASS") != 0 &&
        strcmp(command, "QUIT") != 0) {

        if (!ValidateSession(context)) {
            SendResponse(context, "530 Session expired or invalid. Please login again.\r\n");
            return;
        }
    }

    if (strcmp(command, "REGISTER") == 0) {
        HandleREGISTER(context, param1);
    }
    else if (strcmp(command, "USER") == 0) {
        HandleUSER(context, param1);
    }
    else if (strcmp(command, "PASS") == 0) {
        HandlePASS(context, param1);
    }
    else if (strcmp(command, "LOGOUT") == 0) {
        HandleLOGOUT(context);
    }
    else if (strcmp(command, "WHOAMI") == 0) {
        HandleWHOAMI(context);
    }
    else if (strcmp(command, "USERS") == 0) {
        HandleUSERS(context);
    }
    else if (strcmp(command, "CHPASS") == 0) {
        HandleCHPASS(context, param1);
    }
    else if (strcmp(command, "SYST") == 0) {
        HandleSYST(context);
    }
    else if (strcmp(command, "FEAT") == 0) {
        HandleFEAT(context);
    }
    else if (strcmp(command, "PWD") == 0) {
        HandlePWD(context);
    }
    else if (strcmp(command, "CWD") == 0) {
        HandleCWD(context, param1);
    }
    else if (strcmp(command, "LIST") == 0) {
        HandleLIST(context);
    }
    else if (strcmp(command, "RETR") == 0) {
        HandleRETR(context, param1);
    }
    else if (strcmp(command, "STOR") == 0) {
        HandleSTOR(context, param1);
    }
    else if (strcmp(command, "QUIT") == 0) {
        HandleQUIT(context);
    }
    else if (strcmp(command, "PORT") == 0) {
        HandlePORT(context, param1);
    }
    else if (strcmp(command, "PASV") == 0) {
        HandlePASV(context);
    }
    else if (strcmp(command, "TYPE") == 0) {
        HandleTYPE(context, param1);
    }
    else {
        SendResponse(context, "502 Command not implemented\r\n");
    }
}

// ==================== 用户管理命令 ====================

void CFTPServer::HandleREGISTER(ClientContext* context, const char* param) {
    char username[64] = { 0 };
    char password[64] = { 0 };
    char email[128] = { 0 };

    int result = sscanf_s(param, "%63s %63s %127s",
        username, (unsigned)_countof(username),
        password, (unsigned)_countof(password),
        email, (unsigned)_countof(email));

    if (result < 2) {
        SendResponse(context, "501 Usage: REGISTER <username> <password> [email]\r\n");
        return;
    }

    if (strlen(username) < 3) {
        SendResponse(context, "501 Username must be at least 3 characters\r\n");
        return;
    }

    if (strlen(password) < 6) {
        SendResponse(context, "501 Password must be at least 6 characters\r\n");
        return;
    }

    if (AddUser(username, password, email)) {
        CreateUserDirectory(username);

        char response[512];
        sprintf_s(response, sizeof(response),
            "230 User %s registered successfully. Please login with USER and PASS.\r\n",
            username);
        SendResponse(context, response);

        printf("New user registered: %s\n", username);
    }
    else {
        SendResponse(context, "530 Username already exists or registration failed\r\n");
    }
}

void CFTPServer::HandleUSER(ClientContext* context, const char* param) {
    strcpy_s(context->username, sizeof(context->username), param);
    context->failedAttempts = 0;

    if (IsUserLocked(param)) {
        SendResponse(context, "530 Account is locked due to too many failed attempts. Please try again later.\r\n");
        return;
    }

    SendResponse(context, "331 User name okay, need password\r\n");
}

void CFTPServer::HandlePASS(ClientContext* context, const char* param) {
    int userId = 0;

    if (AuthenticateUser(context->username, param, &userId)) {
        context->userId = userId;

        if (GenerateSessionToken(context)) {
            context->state = STATE_LOGGED_IN;
            context->isAuthenticated = TRUE;

            GetUserHomeDirectory(context->currentDir, context->username);

            UpdateLoginInfo(userId, TRUE);

            char response[512];
            sprintf_s(response, sizeof(response),
                "230 User %s logged in successfully. Session token: %s\r\n",
                context->username, context->sessionToken);
            SendResponse(context, response);

            printf("User logged in: %s (Client %d, User ID: %d)\n",
                context->username, context->clientId, userId);
        }
        else {
            SendResponse(context, "530 Failed to generate session token\r\n");
        }
    }
    else {
        context->failedAttempts++;

        if (IsUserLocked(context->username)) {
            SendResponse(context, "530 Account has been locked due to too many failed attempts\r\n");
        }
        else {
            int remaining = MAX_PASSWORD_ATTEMPTS - context->failedAttempts;
            if (remaining > 0) {
                char response[256];
                sprintf_s(response, sizeof(response),
                    "530 Login incorrect. %d attempt(s) remaining.\r\n", remaining);
                SendResponse(context, response);
            }
            else {
                SendResponse(context, "530 Login incorrect. Account locked.\r\n");
            }
        }

        UpdateLoginInfo(0, FALSE);
    }
}

void CFTPServer::HandleLOGOUT(ClientContext* context) {
    if (context->isAuthenticated) {
        printf("User logout: %s (Client %d)\n", context->username, context->clientId);
        InvalidateSession(context);
        context->isAuthenticated = FALSE;
        context->state = STATE_NOT_LOGGED_IN;
        memset(context->username, 0, sizeof(context->username));
        memset(context->sessionToken, 0, sizeof(context->sessionToken));
        SendResponse(context, "221 Goodbye. Logged out successfully.\r\n");
    }
    else {
        SendResponse(context, "530 Not logged in\r\n");
    }
}

void CFTPServer::HandleWHOAMI(ClientContext* context) {
    if (context->isAuthenticated) {
        char response[512];
        time_t now = time(NULL);
        int sessionAge = (int)(now - context->lastActivityTime);

        sprintf_s(response, sizeof(response),
            "211- User information:\r\n"
            "211-   Username: %s\r\n"
            "211-   User ID: %d\r\n"
            "211-   Session token: %s\r\n"
            "211-   Session age: %d seconds\r\n"
            "211-   Current directory: %s\r\n"
            "211 End\r\n",
            context->username, context->userId, context->sessionToken,
            sessionAge, context->currentDir);
        SendResponse(context, response);
    }
    else {
        SendResponse(context, "530 Not logged in\r\n");
    }
}

void CFTPServer::HandleUSERS(ClientContext* context) {
    if (!context->isAuthenticated) {
        SendResponse(context, "530 Not logged in\r\n");
        return;
    }

    char response[BUFFER_SIZE] = "211- Online users:\r\n";
    int count = 0;

    EnterCriticalSection(&m_sessionCs);
    for (const auto& pair : m_sessions) {
        if (pair.second.isValid) {
            char line[256];
            sprintf_s(line, sizeof(line), "211-   %s (ID: %d, idle: %ds)\r\n",
                pair.second.username,
                pair.second.userId,
                (int)(time(NULL) - pair.second.lastActiveTime));
            strcat_s(response, sizeof(response), line);
            count++;
        }
    }
    LeaveCriticalSection(&m_sessionCs);

    char footer[64];
    sprintf_s(footer, sizeof(footer), "211- Total: %d users online\r\n211 End\r\n", count);
    strcat_s(response, sizeof(response), footer);

    SendResponse(context, response);
}

void CFTPServer::HandleCHPASS(ClientContext* context, const char* param) {
    if (!ValidateSession(context)) {
        SendResponse(context, "530 Please login first\r\n");
        return;
    }

    char oldPassword[64] = { 0 };
    char newPassword[64] = { 0 };

    int result = sscanf_s(param, "%63s %63s",
        oldPassword, (unsigned)_countof(oldPassword),
        newPassword, (unsigned)_countof(newPassword));

    if (result < 2) {
        SendResponse(context, "501 Usage: CHPASS <old_password> <new_password>\r\n");
        return;
    }

    if (strlen(newPassword) < 6) {
        SendResponse(context, "501 New password must be at least 6 characters\r\n");
        return;
    }

    int userId = 0;
    if (AuthenticateUser(context->username, oldPassword, &userId)) {
        if (UpdatePassword(context->userId, newPassword)) {
            SendResponse(context, "200 Password changed successfully\r\n");
            printf("Password changed for user: %s\n", context->username);
        }
        else {
            SendResponse(context, "500 Failed to change password\r\n");
        }
    }
    else {
        SendResponse(context, "530 Old password is incorrect\r\n");
    }
}

// ==================== FTP命令 ====================

void CFTPServer::HandleSYST(ClientContext* context) {
    SendResponse(context, "215 Windows_NT\r\n");
}

void CFTPServer::HandleFEAT(ClientContext* context) {
    SendResponse(context,
        "211-Features:\r\n"
        " SIZE\r\n"
        " MDTM\r\n"
        " REGISTER\r\n"
        " LOGOUT\r\n"
        " WHOAMI\r\n"
        " USERS\r\n"
        " CHPASS\r\n"
        "211 End\r\n");
}

void CFTPServer::HandlePWD(ClientContext* context) {
    char response[512];
    sprintf_s(response, sizeof(response), "257 \"%s\" is current directory\r\n", context->currentDir);
    SendResponse(context, response);
}

void CFTPServer::HandleCWD(ClientContext* context, const char* param) {
    char fullPath[MAX_PATH];

    if (param[0] == '/' || param[0] == '\\') {
        sprintf_s(fullPath, sizeof(fullPath), "C:\\FTPServer\\users\\%s%s", context->username, param);
    }
    else {
        sprintf_s(fullPath, sizeof(fullPath), "%s\\%s", context->currentDir, param);
    }

    char basePath[MAX_PATH];
    GetUserHomeDirectory(basePath, context->username);

    if (!IsPathSafe(basePath, fullPath + strlen(basePath))) {
        SendResponse(context, "550 Access denied\r\n");
        return;
    }

    WCHAR fullPathW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, fullPath, -1, fullPathW, MAX_PATH);

    if (SetCurrentDirectoryW(fullPathW)) {
        WCHAR currentDirW[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, currentDirW);
        WideCharToMultiByte(CP_ACP, 0, currentDirW, -1, context->currentDir, MAX_PATH, NULL, NULL);
        SendResponse(context, "250 Directory successfully changed\r\n");
    }
    else {
        SendResponse(context, "550 Failed to change directory\r\n");
    }
}

void CFTPServer::HandleLIST(ClientContext* context) {
    if (!context->isAuthenticated) {
        SendResponse(context, "530 Not logged in\r\n");
        return;
    }

    if (context->dataSocket == INVALID_SOCKET && !context->isDataConnected) {
        SendResponse(context, "425 No data connection\r\n");
        return;
    }

    SendResponse(context, "150 Opening data connection for directory list\r\n");

    char command[MAX_PATH + 20];
    sprintf_s(command, sizeof(command), "cmd /c dir \"%s\"", context->currentDir);

    FILE* pipe = _popen(command, "r");
    if (pipe == NULL) {
        SendResponse(context, "450 Failed to get directory listing\r\n");
        CloseDataConnection(context);
        return;
    }

    char buffer[BUFFER_SIZE];
    size_t bytesRead;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer) - 1, pipe)) > 0) {
        send(context->dataSocket, buffer, (int)bytesRead, 0);
    }

    _pclose(pipe);

    SendResponse(context, "226 Directory send OK\r\n");
    CloseDataConnection(context);
}

void CFTPServer::HandleRETR(ClientContext* context, const char* param) {
    if (!context->isAuthenticated) {
        SendResponse(context, "530 Not logged in\r\n");
        return;
    }

    if (context->dataSocket == INVALID_SOCKET && !context->isDataConnected) {
        SendResponse(context, "425 No data connection\r\n");
        return;
    }

    char filepath[MAX_PATH];
    sprintf_s(filepath, sizeof(filepath), "%s\\%s", context->currentDir, param);

    char basePath[MAX_PATH];
    GetUserHomeDirectory(basePath, context->username);

    if (!IsPathSafe(basePath, filepath)) {
        SendResponse(context, "550 Access denied\r\n");
        CloseDataConnection(context);
        return;
    }

    WCHAR filepathW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, filepath, -1, filepathW, MAX_PATH);

    HANDLE file = CreateFileW(filepathW, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        char response[256];
        sprintf_s(response, sizeof(response), "550 Failed to open file: %s\r\n", param);
        SendResponse(context, response);
        CloseDataConnection(context);
        return;
    }

    DWORD fileSize = GetFileSize(file, NULL);
    char response[256];
    sprintf_s(response, sizeof(response), "150 Opening binary data connection for %s (%lu bytes)\r\n", param, fileSize);
    SendResponse(context, response);

    context->transferMode = MODE_DOWNLOAD;
    context->fileHandle = file;
    context->bytesToSend = fileSize;
    context->bytesSent = 0;

    StartFileTransfer(context, param, TRUE);
}

void CFTPServer::HandleSTOR(ClientContext* context, const char* param) {
    if (!context->isAuthenticated) {
        SendResponse(context, "530 Not logged in\r\n");
        return;
    }

    if (context->dataSocket == INVALID_SOCKET && !context->isDataConnected) {
        SendResponse(context, "425 No data connection\r\n");
        return;
    }

    char filepath[MAX_PATH];
    sprintf_s(filepath, sizeof(filepath), "%s\\%s", context->currentDir, param);

    char basePath[MAX_PATH];
    GetUserHomeDirectory(basePath, context->username);

    if (!IsPathSafe(basePath, filepath)) {
        SendResponse(context, "550 Access denied\r\n");
        CloseDataConnection(context);
        return;
    }

    WCHAR filepathW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, filepath, -1, filepathW, MAX_PATH);

    HANDLE file = CreateFileW(filepathW, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        char response[256];
        sprintf_s(response, sizeof(response), "550 Failed to create file: %s\r\n", param);
        SendResponse(context, response);
        CloseDataConnection(context);
        return;
    }

    SendResponse(context, "150 Opening binary data connection for file upload\r\n");

    context->transferMode = MODE_UPLOAD;
    context->fileHandle = file;
    context->bytesReceived = 0;

    DWORD flags = 0;
    DWORD recvBytes = 0;
    ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));
    context->wsabuf.len = BUFFER_SIZE;
    context->wsabuf.buf = context->buffer;

    if (WSARecv(context->dataSocket, &context->wsabuf, 1, &recvBytes, &flags, &context->overlapped, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) {
            printf("WSARecv for file upload failed: %d\n", WSAGetLastError());
            CloseHandle(file);
            CloseDataConnection(context);
        }
    }
}

void CFTPServer::HandleQUIT(ClientContext* context) {
    if (context->isAuthenticated) {
        InvalidateSession(context);
    }
    SendResponse(context, "221 Goodbye\r\n");
    closesocket(context->socket);
    RemoveClientContext(context);
}

void CFTPServer::HandlePORT(ClientContext* context, const char* param) {
    int h1, h2, h3, h4, p1, p2;
    sscanf_s(param, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2);

    context->dataAddr.sin_family = AF_INET;
    context->dataAddr.sin_addr.s_addr = htonl((h1 << 24) | (h2 << 16) | (h3 << 8) | h4);
    context->dataAddr.sin_port = htons((p1 << 8) | p2);

    context->dataSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

    SendResponse(context, "200 PORT command successful\r\n");

    if (WSAConnect(context->dataSocket, (sockaddr*)&context->dataAddr, sizeof(context->dataAddr), NULL, NULL, NULL, NULL) == SOCKET_ERROR) {
        printf("Data connection failed: %d\n", WSAGetLastError());
        CloseDataConnection(context);
    }
    else {
        context->isDataConnected = TRUE;
        printf("Data connection established for client %d\n", context->clientId);
    }
}

void CFTPServer::HandlePASV(ClientContext* context) {
    for (int port = DATA_PORT_START; port <= DATA_PORT_END; port++) {
        SOCKET dataListen = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

        sockaddr_in dataAddr;
        dataAddr.sin_family = AF_INET;
        dataAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        dataAddr.sin_port = htons(port);

        if (bind(dataListen, (sockaddr*)&dataAddr, sizeof(dataAddr)) == 0) {
            listen(dataListen, 1);

            char response[256];
            char hostname[256];
            gethostname(hostname, sizeof(hostname));

            struct addrinfo hints, * res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            if (getaddrinfo(hostname, "0", &hints, &res) == 0) {
                struct sockaddr_in* addr = (struct sockaddr_in*)res->ai_addr;
                unsigned long ip = ntohl(addr->sin_addr.s_addr);

                int p1 = port / 256;
                int p2 = port % 256;

                sprintf_s(response, sizeof(response),
                    "227 Entering Passive Mode (%lu,%lu,%lu,%lu,%d,%d)\r\n",
                    (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                    p1, p2);

                freeaddrinfo(res);
            }
            else {
                sprintf_s(response, sizeof(response),
                    "227 Entering Passive Mode (127,0,0,1,%d,%d)\r\n",
                    port / 256, port % 256);
            }

            SendResponse(context, response);

            sockaddr_in clientAddr;
            int addrLen = sizeof(clientAddr);
            context->dataSocket = accept(dataListen, (sockaddr*)&clientAddr, &addrLen);
            context->isDataConnected = TRUE;

            closesocket(dataListen);
            printf("PASV data connection established for client %d\n", context->clientId);
            return;
        }
        closesocket(dataListen);
    }

    SendResponse(context, "425 Can't open passive connection\r\n");
}

void CFTPServer::HandleTYPE(ClientContext* context, const char* param) {
    if (param[0] == 'I' || param[0] == 'i') {
        SendResponse(context, "200 Type set to binary\r\n");
    }
    else {
        SendResponse(context, "200 Type set to ASCII\r\n");
    }
}

// ==================== 辅助函数 ====================

void CFTPServer::CloseDataConnection(ClientContext* context) {
    if (context->dataSocket != INVALID_SOCKET) {
        closesocket(context->dataSocket);
        context->dataSocket = INVALID_SOCKET;
    }
    context->isDataConnected = FALSE;

    if (context->fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(context->fileHandle);
        context->fileHandle = INVALID_HANDLE_VALUE;
    }

    context->transferMode = MODE_NONE;
}

void CFTPServer::StartFileTransfer(ClientContext* context, const char* filename, BOOL isDownload) {
    if (isDownload) {
        DWORD flags = 0;
        DWORD sentBytes = 0;
        ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));

        DWORD toRead = min(BUFFER_SIZE, context->bytesToSend);
        ReadFile(context->fileHandle, context->buffer, toRead, NULL, &context->overlapped);

        context->wsabuf.len = toRead;
        context->wsabuf.buf = context->buffer;

        if (WSASend(context->dataSocket, &context->wsabuf, 1, &sentBytes, flags, &context->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                printf("WSASend for file transfer failed: %d\n", WSAGetLastError());
                CloseDataConnection(context);
            }
        }
    }
}

void CFTPServer::ContinueFileTransfer(ClientContext* context, DWORD bytesTransferred) {
    if (context->transferMode == MODE_DOWNLOAD) {
        context->bytesSent += bytesTransferred;

        if (context->bytesSent < context->bytesToSend) {
            DWORD flags = 0;
            DWORD sentBytes = 0;
            DWORD toSend = min(BUFFER_SIZE, context->bytesToSend - context->bytesSent);

            ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));

            ReadFile(context->fileHandle, context->buffer, toSend, NULL, &context->overlapped);

            context->wsabuf.len = toSend;
            context->wsabuf.buf = context->buffer;

            if (WSASend(context->dataSocket, &context->wsabuf, 1, &sentBytes, flags, &context->overlapped, NULL) == SOCKET_ERROR) {
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    printf("WSASend continue failed: %d\n", WSAGetLastError());
                    CloseDataConnection(context);
                }
            }
        }
        else {
            SendResponse(context, "226 Transfer complete\r\n");
            CloseDataConnection(context);
        }
    }
    else if (context->transferMode == MODE_UPLOAD) {
        DWORD written;
        WriteFile(context->fileHandle, context->buffer, bytesTransferred, &written, NULL);
        context->bytesReceived += bytesTransferred;

        DWORD flags = 0;
        DWORD recvBytes = 0;
        ZeroMemory(&context->overlapped, sizeof(OVERLAPPED));
        context->wsabuf.len = BUFFER_SIZE;
        context->wsabuf.buf = context->buffer;

        if (WSARecv(context->dataSocket, &context->wsabuf, 1, &recvBytes, &flags, &context->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                SendResponse(context, "226 Transfer complete\r\n");
                CloseDataConnection(context);
            }
        }
    }
}

void CFTPServer::ProcessDataConnection(ClientContext* context) {
    ContinueFileTransfer(context, context->bytesTransferred);
}

void CFTPServer::SendResponse(ClientContext* context, const char* response) {
    send(context->socket, response, (int)strlen(response), 0);
    printf("Response to %d: %s", context->clientId, response);
}

void CFTPServer::CreateUserDirectory(const char* username) {
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "C:\\FTPServer\\users\\%s", username);

    WCHAR pathW[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, pathW, MAX_PATH);

    CreateDirectoryW(pathW, NULL);

    WCHAR subDirW[MAX_PATH];
    wcscpy_s(subDirW, pathW);
    wcscat_s(subDirW, L"\\uploads");
    CreateDirectoryW(subDirW, NULL);

    wcscpy_s(subDirW, pathW);
    wcscat_s(subDirW, L"\\downloads");
    CreateDirectoryW(subDirW, NULL);
}

void CFTPServer::GetUserHomeDirectory(char* path, const char* username) {
    sprintf_s(path, MAX_PATH, "C:\\FTPServer\\users\\%s", username);
}

BOOL CFTPServer::IsPathSafe(const char* basePath, const char* requestedPath) {
    char fullPath[MAX_PATH];
    sprintf_s(fullPath, sizeof(fullPath), "%s", requestedPath);

    char canonicalPath[MAX_PATH];
    if (!GetFullPathNameA(fullPath, MAX_PATH, canonicalPath, NULL)) {
        return FALSE;
    }

    return strncmp(canonicalPath, basePath, strlen(basePath)) == 0;
}

ClientContext* CFTPServer::CreateClientContext(SOCKET clientSocket) {
    if (m_clientCount >= MAX_CLIENTS) {
        printf("Max clients reached\n");
        return NULL;
    }

    ClientContext* context = new ClientContext();
    ZeroMemory(context, sizeof(ClientContext));

    context->socket = clientSocket;
    context->dataSocket = INVALID_SOCKET;
    context->state = STATE_NOT_LOGGED_IN;
    context->transferMode = MODE_NONE;
    context->fileHandle = INVALID_HANDLE_VALUE;
    context->isDataConnected = FALSE;
    context->bytesTransferred = 0;
    context->clientId = m_clientCount + 1;
    context->lastActivityTime = time(NULL);
    context->isAuthenticated = FALSE;
    context->failedAttempts = 0;
    memset(context->sessionToken, 0, sizeof(context->sessionToken));

    EnterCriticalSection(&m_cs);
    m_clients[m_clientCount++] = context;
    LeaveCriticalSection(&m_cs);

    return context;
}

void CFTPServer::RemoveClientContext(ClientContext* context) {
    if (context == NULL) return;

    if (context->isAuthenticated) {
        InvalidateSession(context);
    }

    EnterCriticalSection(&m_cs);
    for (int i = 0; i < m_clientCount; i++) {
        if (m_clients[i] == context) {
            if (context->dataSocket != INVALID_SOCKET) {
                closesocket(context->dataSocket);
            }
            if (context->fileHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(context->fileHandle);
            }

            printf("Client %d disconnected\n", context->clientId);

            delete context;

            for (int j = i; j < m_clientCount - 1; j++) {
                m_clients[j] = m_clients[j + 1];
            }
            m_clientCount--;
            break;
        }
    }
    LeaveCriticalSection(&m_cs);
}

ClientContext* CFTPServer::FindClientBySocket(SOCKET socket) {
    EnterCriticalSection(&m_cs);
    for (int i = 0; i < m_clientCount; i++) {
        if (m_clients[i]->socket == socket) {
            LeaveCriticalSection(&m_cs);
            return m_clients[i];
        }
    }
    LeaveCriticalSection(&m_cs);
    return NULL;
}

ClientContext* CFTPServer::FindClientByUsername(const char* username) {
    EnterCriticalSection(&m_cs);
    for (int i = 0; i < m_clientCount; i++) {
        if (m_clients[i]->isAuthenticated && strcmp(m_clients[i]->username, username) == 0) {
            LeaveCriticalSection(&m_cs);
            return m_clients[i];
        }
    }
    LeaveCriticalSection(&m_cs);
    return NULL;
}

void CFTPServer::Shutdown() {
    if (m_sessionMonitorThread != NULL) {
        TerminateThread(m_sessionMonitorThread, 0);
        CloseHandle(m_sessionMonitorThread);
    }

    EnterCriticalSection(&m_cs);
    for (int i = 0; i < m_clientCount; i++) {
        if (m_clients[i]->socket != INVALID_SOCKET) {
            closesocket(m_clients[i]->socket);
        }
        if (m_clients[i]->dataSocket != INVALID_SOCKET) {
            closesocket(m_clients[i]->dataSocket);
        }
        if (m_clients[i]->fileHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_clients[i]->fileHandle);
        }
        delete m_clients[i];
    }
    m_clientCount = 0;
    LeaveCriticalSection(&m_cs);

    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
    }

    if (m_completionPort != NULL) {
        CloseHandle(m_completionPort);
    }

    for (int i = 0; i < m_threadCount; i++) {
        if (m_workerThreads[i] != NULL) {
            TerminateThread(m_workerThreads[i], 0);
            CloseHandle(m_workerThreads[i]);
        }
    }

    if (m_db) {
        sqlite3_close(m_db);
        m_db = NULL;
    }

    WSACleanup();
    printf("Server shutdown complete\n");
}