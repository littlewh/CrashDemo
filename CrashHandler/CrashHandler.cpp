#include "CrashHandler.h"
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <thread>
#include <atomic>
#include <shlobj.h>
#include <tlhelp32.h>   // 用于线程快照

#pragma comment(lib, "dbghelp.lib")

// 全局配置
static CrashHandler::DumpLevel g_DumpLevel = CrashHandler::DumpLevel::Normal;
static std::wstring g_ReporterPath;
static HANDLE g_WatchdogThread = nullptr;
static bool g_EnableWatchdog = true;

// 生成 MINIDUMP_TYPE
MINIDUMP_TYPE CrashHandler::GetDumpType(const CrashHandler::DumpLevel& i_emLevel)
{
    switch (i_emLevel)
    {
    case CrashHandler::DumpLevel::Light:
        return (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | //包含栈上的指针所指向的内存（展开调用栈）
            MiniDumpWithThreadInfo |                                    //包含线程名称、线程时间、优先级等元数据
            MiniDumpWithModuleHeaders |                                 //包含模块的完整PE头信息（版本、时间戳、基址等）    
            MiniDumpWithUnloadedModules);                               //包含曾经加载但已卸载的 DLL 信息
    case CrashHandler::DumpLevel::Normal:
        return (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | //包含栈上的指针所指向的内存（展开调用栈）
            MiniDumpWithThreadInfo |                                    //包含线程名称、线程时间、优先级等元数据
            MiniDumpWithModuleHeaders |                                 //包含模块的完整PE头信息（版本、时间戳、基址等）    
            MiniDumpWithUnloadedModules |                               //包含曾经加载但已卸载的 DLL 信息
            MiniDumpWithPrivateReadWriteMemory |                        //包含进程私有内存区域（堆、栈、全局变量）的可读写页面（局部变量、类成员变量、动态分配对象的实际内容）
            MiniDumpWithDataSegs |                                      //包含 EXE/DLL 的 .data 和 .rdata 段（已初始化的全局/静态变量）
            MiniDumpWithHandleData |                                    //包含进程句柄表的信息（文件句柄、事件、互斥体等，分析句柄泄漏、死锁）
            MiniDumpWithFullMemoryInfo );                               //包含完整的内存区域属性列表（哪些地址范围是可读/可写/可执行的）
    case CrashHandler::DumpLevel::Full:
        return (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | //包含栈上的指针所指向的内存（展开调用栈）
            MiniDumpWithThreadInfo |                                    //包含线程名称、线程时间、优先级等元数据
            MiniDumpWithModuleHeaders |                                 //包含模块的完整PE头信息（版本、时间戳、基址等）    
            MiniDumpWithUnloadedModules |                               //包含曾经加载但已卸载的 DLL 信息
            MiniDumpWithPrivateReadWriteMemory |                        //包含进程私有内存区域（堆、栈、全局变量）的可读写页面（局部变量、类成员变量、动态分配对象的实际内容）
            MiniDumpWithDataSegs |                                      //包含 EXE/DLL 的 .data 和 .rdata 段（已初始化的全局/静态变量）
            MiniDumpWithHandleData |                                    //包含进程句柄表的信息（文件句柄、事件、互斥体等，分析句柄泄漏、死锁）
            MiniDumpWithFullMemoryInfo |                                //包含完整的内存区域属性列表（哪些地址范围是可读/可写/可执行的）
            MiniDumpWithFullMemory |                                    //转储进程的全部虚拟内存空间（包括未使用的保留区域和所有私有页面）
            MiniDumpWithTokenInformation);                              //包含进程的安全令牌信息（用户权限等）                          
    default:
        return MiniDumpNormal;
    }
}

// 获取 Dump 保存路径（%LOCALAPPDATA%\YourCAD\Crashes\）
static std::wstring GetDumpPath(DWORD exceptionCode)
{
    wchar_t path[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path);
    wcscat_s(path, L"\\CrashDemo_Final_Crashes");
    CreateDirectoryW(path, nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(path, L"%s\\Crash_v1.0_%04d%02d%02d_%02d%02d%02d_0x%08X.dmp",
        path, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, exceptionCode);
    return path;
}

// 挂起除当前线程外的所有线程
static void SuspendOtherThreads(DWORD selfThreadId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(hSnapshot, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == GetCurrentProcessId() &&
                te.th32ThreadID != selfThreadId)
            {
                HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (hThread)
                {
                    SuspendThread(hThread);
                    CloseHandle(hThread);
                }
            }
        } while (Thread32Next(hSnapshot, &te));
    }
    CloseHandle(hSnapshot);
}

#include <fstream>
#include <iostream>
// 独立 Reporter 进程启动函数（供外部调用）
void CrashHandler::GenerateCrashDump(DWORD exceptionCode)
{
    static std::atomic<bool> dumping{ false };
    if (dumping.exchange(true))
        return;

    // 1. 挂起所有其他线程，冻结进程状态
    SuspendOtherThreads(GetCurrentThreadId());

    if (g_ReporterPath.empty())
    {
        wprintf(L"[CrashHandler] Reporter path not set, cannot generate dump.\n");
        return;
    }

    wchar_t cmd[1024];
    swprintf_s(cmd, L"\"%s\" --pid=%u --code=0x%08X --level=%d",
        g_ReporterPath.c_str(),
        GetCurrentProcessId(),
        exceptionCode,
        (int)g_DumpLevel);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS,
        nullptr, nullptr, &si, &pi))
    {
        if (pi.hProcess)
        {
            WaitForSingleObject(pi.hProcess, 15000);  // 融合 B 的 15 秒等待
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    // 3. 强制终止进程
    TerminateProcess(GetCurrentProcess(), 0xDEAD0003);
}

// 内部调用：从异常信息启动 Reporter
static void LaunchReporter(EXCEPTION_POINTERS* exInfo)
{
    DWORD code = exInfo ? exInfo->ExceptionRecord->ExceptionCode : 0xFFFFFFFF;
    CrashHandler::GenerateCrashDump(code);
}

// ==================== 各种死亡路径处理器 ====================

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* ex)
{
	if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) // 调试断点，不处理
        return EXCEPTION_CONTINUE_SEARCH;
    LaunchReporter(ex);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ex)
{
    LaunchReporter(ex);
    return EXCEPTION_EXECUTE_HANDLER;
}

void PureCallHandler()
{
    LaunchReporter(nullptr);
    abort();
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    LaunchReporter(nullptr);
    abort();
}

void TerminateHandler()
{
    EXCEPTION_RECORD rec = { STATUS_FATAL_APP_EXIT };
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_POINTERS ptr{ &rec, &ctx };
    LaunchReporter(&ptr);
    abort();
}

void SignalHandler(int)
{
    LaunchReporter(nullptr);
    _exit(1);
}

// 栈溢出专用处理器（借助备用栈）
LONG CALLBACK StackOverflowHandler(EXCEPTION_POINTERS* ex)
{
    if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        LaunchReporter(ex);
        TerminateProcess(GetCurrentProcess(), 0xDEAD0002);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ==================== Watchdog 线程（第二重保障）====================

static std::atomic<uint64_t> g_MainThreadHeartbeat{ 0 };
static const uint64_t WATCHDOG_TIMEOUT_MS = 15000;
// 主线程定期报告心跳（在主循环中调用）
void CrashHandler::UpdateHeartbeat()
{
    g_MainThreadHeartbeat.store(GetTickCount64());
}
// Watchdog线程（增强版）
static DWORD WINAPI WatchdogProc(LPVOID)
{
    while (true)
    {
        Sleep(3000);
        uint64_t last = g_MainThreadHeartbeat.load();
        if (last > 0 && (GetTickCount64() - last) > WATCHDOG_TIMEOUT_MS)
        {
            wprintf(L"[Watchdog] 检测到主线程卡死/死循环，强制生成Dump!\n");
            EXCEPTION_RECORD rec = { STATUS_FATAL_APP_EXIT };
            CONTEXT ctx = {};
            RtlCaptureContext(&ctx);
            EXCEPTION_POINTERS ptr = { &rec, &ctx };
            LaunchReporter(&ptr);
            TerminateProcess(GetCurrentProcess(), 0xDEAD0003);
        }
    }
    return 0;
}

// ==================== 安装函数 ====================
void CrashHandler::Install(const std::wstring& reporterPath, DumpLevel level, bool enableWatchdog)
{
    static bool installed = false;
    if (installed)
    {
        wprintf(L"[CrashHandler] Already installed, skipping.\n");
        return;
    }
    installed = true;

    g_DumpLevel = level;
    g_ReporterPath = reporterPath.empty() ? L"CrashReporter.exe" : reporterPath;
    g_EnableWatchdog = enableWatchdog;

    // 1. 预留栈溢出备用空间（64KB）
	//  用以发生栈溢出时，仍能在备用栈上执行异常处理器，生成 Dump
    ULONG guaranteeSize = 65536;
    SetThreadStackGuarantee(&guaranteeSize);

    // 2. 安装向量化异常处理器（含栈溢出专用）
	//  VEH 的优先级高于 UEF，可以捕获更多异常（如栈溢出），并且不受当前模块的 SEH 影响
    AddVectoredExceptionHandler(1, VectoredHandler);
    AddVectoredExceptionHandler(1, StackOverflowHandler);

    // 3. 未处理异常过滤器
	//  UEF 的最后一道防线，捕获所有未被前面处理器捕获的异常
    SetUnhandledExceptionFilter(TopLevelFilter);

    // 4. C++ 运行时钩子
	//  捕获纯虚函数调用、无效参数、std::terminate 导致的崩溃等 C++ 运行时错误
    std::set_terminate(TerminateHandler);
    _set_purecall_handler(PureCallHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // 5. C 信号
	//  捕获 SIGABRT、SIGTERM 等信号引发的崩溃
    signal(SIGABRT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 6. 禁用 Windows 错误报告弹窗
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // 7. 启动看门狗线程（可选）
	//  看门狗线程定期检查主线程心跳，如果主线程长时间无响应（可能死循环或卡死），强制生成 Dump
    if (g_EnableWatchdog)
    {
        g_WatchdogThread = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
    }

    wprintf(L"[CrashHandler] Installed. Reporter: %ls, Level: %d, Watchdog: %s\n",
        g_ReporterPath.c_str(), (int)level, enableWatchdog ? "ON" : "OFF");
}

void CrashHandler::SetDumpLevel(DumpLevel level)
{
    g_DumpLevel = level;
}

std::wstring CrashHandler::GetDumpDirectory()
{
    wchar_t path[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path);
    wcscat_s(path, L"\\CrashDemo_Final_Crashes");
    return path;
}