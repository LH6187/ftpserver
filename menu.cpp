#define NOMINMAX
#include "menu.h"
#include "log_module.h"
#include "config_module.h"
#include <iostream>
#include <limits>

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
            LogModule::viewLogsMenu();
            break;

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