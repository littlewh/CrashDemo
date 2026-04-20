#include <windows.h>
#include <stdexcept>
#include "DynamicPlugin.h"
#include "../CrashHandler/CrashHandler.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // 重新安装处理器（确保覆盖当前模块的异常）
        CrashHandler::Install(L"CrashReporter.exe", CrashHandler::DumpLevel::Normal, true);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void CrashInDLL()
{
    throw std::runtime_error("Uncaught exception in DLL");
}


extern "C" __declspec(dllexport) void DynamicLib_TriggerNullPointer() {
    int* p = nullptr;
    *p = 42; // 空指针写入 → 访问违例
}
