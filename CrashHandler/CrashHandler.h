#pragma once
#include <windows.h>
#include <string>
#include <dbghelp.h>

namespace CrashHandler
{
    enum class DumpLevel
    {
        Light = 1,
        Normal = 2,     // 推荐：包含 HandleData，可分析句柄
        Full = 3
    };

    // 必须最早调用（main 函数第一行或 DllMain 的 PROCESS_ATTACH）
    // reporterPath: CrashReporter.exe 的完整路径，为空则在 exe 同目录查找
    // level: Dump 详细程度
    // enableWatchdog: 是否创建独立监控线程（增强栈溢出场景可靠性）
    extern "C" __declspec(dllexport) void Install(const std::wstring& reporterPath = L"",
        DumpLevel level = DumpLevel::Normal,
        bool enableWatchdog = true);

    extern "C" __declspec(dllexport) void SetDumpLevel(DumpLevel level);
    extern "C" __declspec(dllexport) std::wstring GetDumpDirectory();      // 返回 Dump 保存目录

    // 手动生成当前进程的 Dump（可用于看门狗主动触发）
    extern "C" __declspec(dllexport) void GenerateCrashDump(DWORD exceptionCode = 0xDEAD0000);

    extern "C" __declspec(dllexport) void UpdateHeartbeat();

    extern "C" __declspec(dllexport) MINIDUMP_TYPE GetDumpType(const CrashHandler::DumpLevel& i_emLevel);
}