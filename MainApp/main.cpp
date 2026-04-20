#include "../CrashHandler/CrashHandler.h"
#include "../BusinessLib/BusinessLib.h"
#include "../DynamicPlugin/DynamicPlugin.h"
#include <windows.h>
#include <cstdio>

void PrintMenu()
{
    printf("\n=== CAD Crash Demo (Final Fusion) ===\n");
    printf("1. Null Pointer\n");
    printf("2. Stack Overflow\n");
    printf("3. Pure Virtual Call\n");
    printf("4. Heap Corruption (Use-After-Free)\n");
    printf("5. Uncaught Exception (std::terminate)\n");
    printf("6. Crash in Worker Thread\n");
    printf("7. Crash in DLL\n");
    printf("8. while\n");
    printf("9. DynamicLib_TriggerNullPointer\n");
    printf("0. Exit\n");
    printf("Choice: ");
}

int main()
{
    // 构造 CrashReporter.exe 路径
    wchar_t reporterPath[MAX_PATH];
    GetModuleFileNameW(nullptr, reporterPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(reporterPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = 0;
    wcscat_s(reporterPath, L"CrashReporter.exe");

    // 安装崩溃处理器（融合方案：Normal 级别 + Watchdog 开启）
    CrashHandler::Install(reporterPath, CrashHandler::DumpLevel::Normal, true);

    printf("CrashDemo (Fusion) started.\n");
    printf("Dump will be saved to: %ls\n\n", CrashHandler::GetDumpDirectory().c_str());

    int choice = 0;
    while (true)
    {
        PrintMenu();
        scanf_s("%d", &choice);

        try
        {
            switch (choice)
            {
            case 1: BusinessLib::NullPointer(); break;
            case 2: BusinessLib::StackOverflow(); break;
            case 3: BusinessLib::PureVirtualCall(); break;
            case 4: BusinessLib::HeapCorruption(); break;
            case 5: BusinessLib::UncaughtException(); break;
            case 6: BusinessLib::CrashInWorkerThread(); break;
            case 7:
            {
                // 加载 DLL（其 DllMain 会再次安装，无害）
                HMODULE hDll = LoadLibraryW(L"DynamicPlugin.dll");
                if (!hDll) printf("Warning: DynamicPlugin.dll not found.\n");
                if (hDll)
                {
                    auto fn = (void(*)())GetProcAddress(hDll, "CrashInDLL");
                    if (fn) fn();
                    FreeLibrary(hDll);
                }
            }
                break;
            case 8:
                CrashHandler::UpdateHeartbeat(); // 更新心跳，供看门狗监测
                while (true)
                {
                    int i = 0;
                    i++;
                }
                break;
            case 9:
                DynamicLib_TriggerNullPointer();
                break;
            case 0: return 0;
            default: printf("Invalid choice.\n");
            }
        }
        catch (...)
        {
            // 正常情况下不会到这里，因为各种 handler 会接管
            printf("Caught unexpected exception.\n");
        }
    }

    
    return 0;
}