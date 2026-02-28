#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

using namespace std;

// 日志文件名常量
const string LOG_FILE_NAME = "ftp_server_log.txt";

// 操作类型枚举
enum LogType {
    LOG_UPLOAD,
    LOG_DOWNLOAD,
    LOG_LOGIN,
    LOG_DELETE,
    LOG_ERROR
};

namespace LogModule {

    // ... (getCurrentTime 和 typeToString 保持不变，它们已经是 static 了，没问题) ...
    static string getCurrentTime() {
        time_t now = time(NULL);
        tm ltm;
        localtime_s(&ltm, &now);
        stringstream ss;
        ss << setfill('0')
            << setw(4) << (ltm.tm_year + 1900) << "-"
            << setw(2) << (ltm.tm_mon + 1) << "-"
            << setw(2) << ltm.tm_mday << " "
            << setw(2) << ltm.tm_hour << ":"
            << setw(2) << ltm.tm_min << ":"
            << setw(2) << ltm.tm_sec;
        return ss.str();
    }

    static string typeToString(LogType type) {
        switch (type) {
        case LOG_UPLOAD: return "UPLOAD";
        case LOG_DOWNLOAD: return "DOWNLOAD";
        case LOG_LOGIN: return "LOGIN";
        case LOG_DELETE: return "DELETE";
        default: return "UNKNOWN";
        }
    }

    /**
     * 【修改点 1】添加 inline 关键字
     */
    inline void recordLog(const string& ip, LogType type, const string& filename, const string& status, long size) {
        ofstream outFile(LOG_FILE_NAME.c_str(), ios::app);
        if (!outFile.is_open()) {
            cerr << "[Error] Cannot open log file for writing." << endl;
            return;
        }

        string timeStr = getCurrentTime();
        string typeStr = typeToString(type);

        outFile << "[" << timeStr << "] ";
        outFile << "[" << ip << "] ";
        outFile << "[" << typeStr << "] ";
        outFile << "\"" << filename << "\" ";
        outFile << "[" << status << "] ";
        outFile << "(" << size << " bytes)" << endl;

        outFile.close();
    }

    /**
     * 【修改点 2】添加 inline 关键字
     */
    inline void viewLogsMenu() {
        system("cls");

        cout << endl;
        cout << "========================================" << endl;
        cout << "           FTP Transfer Logs            " << endl;
        cout << "========================================" << endl;

        ifstream inFile(LOG_FILE_NAME.c_str());
        if (!inFile.is_open()) {
            cout << "No log file found yet. No transfers recorded." << endl;
            cout << "Press Enter to return..." << endl;
            cin.ignore();
            cin.get();
            return;
        }

        string line;
        int count = 0;

        cout.setf(ios::left);
        cout.width(20); cout << "Time";
        cout.width(16); cout << "Client IP";
        cout.width(10); cout << "Action";
        cout.width(25); cout << "File";
        cout.width(10); cout << "Status";
        cout << "Size" << endl;

        cout << "---------------------------------------------------------------------------" << endl;

        while (getline(inFile, line)) {
            cout << line << endl;
            count++;
        }
        inFile.close();

        cout << "---------------------------------------------------------------------------" << endl;
        cout << "Total records: " << count << endl;
        cout << "========================================" << endl;
        cout << "Press Enter to return to main menu..." << endl;

        cin.clear();
        cin.ignore(10000, '\n');
        cin.get();
    }
}