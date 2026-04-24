CrashDemo_Final/
├── CrashHandler/                 # 核心崩溃处理
│   ├── include/CrashHandler.h
│   └── src/CrashHandler.cpp
├── CrashReporter/                # 独立进程 - 写 Dump
│   └── src/main.cpp
├── BusinessLib/                  # 静态库 - 模拟崩溃场景
│   ├── include/BusinessLib.h
│   └── src/BusinessLib.cpp
├── DynamicPlugin/                # 动态库 - 测试 DLL 内崩溃
│   ├── include/DynamicPlugin.h
│   └── src/DynamicPlugin.cpp
└──  MainApp/                      # 主程序
     └── src/main.cpp