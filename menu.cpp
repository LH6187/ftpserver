#define NOMINMAX
#include "menu.h"
#include "config_module.h"
#include <iostream>
#include <limits>
#include "db_log_module.h"
#include <iomanip>  
#include <sstream> 

using namespace std;

namespace MenuModule {

    void displayMenu() {
        cout << endl;
        cout << "========================================" << endl;
        cout << "       FTP Server Control Panel         " << endl;
        cout << "========================================" << endl;
        cout << "  [1] View Transfer Logs                " << endl;
        cout << "  [2] Refresh Server Status             " << endl;
        cout << "  [3] Set Listen Address                " << endl;
        cout << "  [0] Exit Server                       " << endl;
        cout << "========================================" << endl;
        cout << "Enter your choice: ";
    }

    MenuOption getUserChoice() {
        int choice = -1;

        while (true) {
            if (!(cin >> choice)) {
                cout << "Invalid input. Please enter a number." << endl;
                cin.clear();
                // 这里也使用同样的清理逻辑
                cin.ignore(numeric_limits<streamsize>::max(), '\n');
                displayMenu();
                continue;
            }

            if (choice == 1 || choice == 2 || choice == 3 || choice == 0) {
                return static_cast<MenuOption>(choice);
            }
            else {
                cout << "Invalid option. Please try again." << endl;
                displayMenu();
            }
        }
    }

    void handleChoice(MenuOption choice) {
        switch (choice) {
        case OPTION_VIEW_LOGS:
        {
            cout << "\n========================================" << endl;
            cout << "           FTP Transfer Logs           " << endl;
            cout << "========================================" << endl;
            cout << left
                << setw(20) << "Time"
                << setw(16) << "Client IP"
                << setw(10) << "Action"
                << setw(30) << "File"
                << setw(10) << "Status"
                << setw(12) << "Size" << endl;
            cout << string(100, '-') << endl;

            // 从数据库查询最新的100条日志
            auto logs = DbLogModule::queryLatestLogs(100);

            if (logs.empty()) {
                cout << "暂无日志记录" << endl;
            }
            else {
                for (const auto& log : logs) {
                    // 格式化文件大小
                    stringstream sizeStr;
                    if (log.fileSize < 1024) {
                        sizeStr << log.fileSize << " B";
                    }
                    else if (log.fileSize < 1024 * 1024) {
                        sizeStr << fixed << setprecision(1) << (log.fileSize / 1024.0) << " KB";
                    }
                    else {
                        sizeStr << fixed << setprecision(1) << (log.fileSize / (1024.0 * 1024.0)) << " MB";
                    }

                    cout << "[" << log.timestamp << "] "
                        << "[" << log.clientIP << "] "
                        << "[" << log.operation << "] "
                        << "\"" << log.filename << "\" "
                        << "[" << log.status << "] "
                        << "(" << sizeStr.str() << ")" << endl;
                }
            }

            cout << string(100, '-') << endl;
            cout << "总计: " << logs.size() << " 条记录" << endl;
            cout << "========================================" << endl;
            break;
        }

        case OPTION_REFRESH_STATUS:
            cout << "\n[Status] Server is running..." << endl;
            ConfigModule::showCurrentConfig();
            break;

        case OPTION_CONFIG_SERVER:
            ConfigModule::setListenAddress();
            break;

        case OPTION_EXIT_SERVER:
            cout << "\nShutting down server..." << endl;
            exit(0);
            break;

        default:
            cout << "Unknown command." << endl;
            break;
        }
    }
}