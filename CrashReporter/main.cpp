#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include "../CrashHandler/CrashHandler.h"

#pragma comment(lib, "dbghelp.lib")

int wmain(int argc, wchar_t* argv[])
{
    DWORD pid = 0;
    DWORD exceptionCode = 0;
    DWORD threadId = 0;
    int level = 2; // 默认 Normal

    for (int i = 1; i < argc; ++i)
    {
        if (wcsstr(argv[i], L"--pid="))    pid = _wtoi(argv[i] + 6);
        if (wcsstr(argv[i], L"--code="))   exceptionCode = wcstoul(argv[i] + 7, nullptr, 16);
        if (wcsstr(argv[i], L"--level="))  level = _wtoi(argv[i] + 8);
        if (wcsstr(argv[i], L"--tid="))    threadId = _wtoi(argv[i] + 6);
    }

    if (!pid)
    {
        wprintf(L"[CrashReporter] Error: No PID provided.\n");
        return 1;
    }

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess)
    {
        wprintf(L"[CrashReporter] Error: Cannot open process %u\n", pid);
        return 1;
    }

    //wchar_t dumpPath[MAX_PATH];
    //swprintf_s(dumpPath, L"Crash_0x%08X_%u.dmp", exceptionCode, GetTickCount());
    // 使用更精确的文件名（包含线程ID）
    wchar_t dumpPath[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    swprintf_s(dumpPath, L"CAD_v1.0_%04d%02d%02d_%02d%02d%02d_tid%u_0x%08X.dmp",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        threadId, exceptionCode);

    HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        wprintf(L"[CrashReporter] Error: Cannot create dump file.\n");
        CloseHandle(hProcess);
        return 1;
    }

    // 根据 level 构造 MINIDUMP_TYPE（与 CrashHandler 保持同步）
    MINIDUMP_TYPE type = CrashHandler::GetDumpType(static_cast<CrashHandler::DumpLevel>(level));

    BOOL success = MiniDumpWriteDump(hProcess, pid, hFile, type, nullptr, nullptr, nullptr);

    DWORD fileSize = GetFileSize(hFile, nullptr);
    wprintf(L"[CrashReporter] Dump written: %s\n", dumpPath);
    wprintf(L"                Success: %d, Size: %lu bytes\n", success, fileSize);

    CloseHandle(hFile);
    CloseHandle(hProcess);
    return success ? 0 : 1;
}