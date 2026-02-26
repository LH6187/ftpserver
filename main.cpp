#include "FTPServer.h"
#include <iostream>

int main() {
    SetConsoleTitle(L"FTP Server with SQLite Database");

    printf("========================================\n");
    printf("    FTP Server with SQLite Database\n");
    printf("========================================\n");

    CFTPServer server;

    if (!server.Initialize(21)) {
        printf("\nFailed to initialize server!\n");
        system("pause");
        return 1;
    }

    printf("\nServer initialized successfully!\n");
    printf("Listening on port 21...\n");
    printf("\nCommands:\n");
    printf("  - Press 'Q' to shutdown server\n");
    printf("\n========================================\n\n");

    server.Run();
    server.Shutdown();

    return 0;
}