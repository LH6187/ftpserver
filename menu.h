#pragma once

// 菜单选项枚举
enum MenuOption {
    OPTION_VIEW_LOGS = 1,
    OPTION_REFRESH_STATUS = 2,
    OPTION_CONFIG_SERVER = 3, // 新增
    OPTION_EXIT_SERVER = 0
};

// 函数声明
namespace MenuModule {
    // 显示主菜单界面
    void displayMenu();

    // 获取用户输入并返回选项
    // 如果输入无效，会提示并重新获取，直到返回有效选项
    MenuOption getUserChoice();

    // 执行对应的操作（调用其他模块的功能）
    // 需要传入日志模块的查看函数指针或直接在内部调用
    void handleChoice(MenuOption choice);
}