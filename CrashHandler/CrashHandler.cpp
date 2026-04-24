#include "CrashHandler.h"
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <thread>
#include <atomic>
#include <shlobj.h>
#include <tlhelp32.h>   // 用于线程快照
#include <iostream>

#pragma comment(lib, "dbghelp.lib")

// 全局配置
static CrashHandler::DumpLevel g_DumpLevel = CrashHandler::DumpLevel::Normal; // Dump文件 详细级别
static std::wstring g_ReporterPath; // Reporter 可执行文件路径（默认为当前目录下的 CrashReporter.exe）
static HANDLE g_WatchdogThread = nullptr; // 看门狗线程句柄
static bool g_EnableWatchdog = true; // 是否启用看门狗线程
//static HANDLE g_CrashMutex = nullptr;           // 命名互斥体
//static std::atomic<DWORD> g_CrashingThreadId{ 0 }; //记录第一个崩溃线程ID，确保只挂起其他线程一次
static std::atomic<bool> g_GlobalInitDone{ false };//初始化标志，确保全局只初始化一次

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

// ====================== 挂起其他线程（关键） ======================
static void SuspendOtherThreads(DWORD crashingThreadId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(hSnapshot, &te))
    {
        do
        {
            if (te.th32OwnerProcessID == GetCurrentProcessId() &&
                te.th32ThreadID != crashingThreadId)
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

// 独立 Reporter 进程启动函数（供外部调用）
void CrashHandler::GenerateCrashDump(DWORD exceptionCode)
{
    DWORD currentThreadId = GetCurrentThreadId();
    // 记录第一个崩溃的线程ID
    /*由于修改原子变量的控制方案后，多线程同时崩溃均可独立捕捉
        暂时取消以下挂起操作*/
    //DWORD expected = 0;
    //if (g_CrashingThreadId.compare_exchange_strong(expected, currentThreadId))
    //{
    //    // 第一个崩溃线程：挂起所有其他线程，防止进一步破坏
    //    SuspendOtherThreads(currentThreadId);
    //    wprintf(L"[CrashHandler] First crashing thread: %u. Suspending others.\n", currentThreadId);
    //}

    /*由于多个线程的dump文件可能不同
        为保证每个线程都能够写出自己的Dump文件
        Mutex不再使用，否则只生成部分Dump文件*/
    //// 使用命名Mutex确保只有一个Reporter进程在写Dump
    //if (WaitForSingleObject(g_CrashMutex, 8000) != WAIT_OBJECT_0)
    //{//等待8秒，如果拿不到锁，说明已有Reporter在运行，直接退出
    //    // 拿不到锁，说明已有Reporter在运行，直接退出
    //    return;
    //}


    if (g_ReporterPath.empty())
    {
        wprintf(L"[CrashHandler] Reporter path not set, cannot generate dump.\n");
        return;
    }

    wchar_t cmd[1024];
    swprintf_s(cmd, L"\"%s\" --pid=%u --code=0x%08X --level=%d --tid=%u",
        g_ReporterPath.c_str(),
        GetCurrentProcessId(),
        exceptionCode,
        (int)g_DumpLevel,
        currentThreadId);

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS,
        nullptr, nullptr, &si, &pi))
    {
        if (pi.hProcess)
        {
			WaitForSingleObject(pi.hProcess, 20000);  // 等待Reporter最多20秒，防止僵尸进程
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
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
#ifdef _DEBUG
    std::cout << "VEH called! Code: 0x" << std::hex << ex->ExceptionRecord->ExceptionCode << std::dec << std::endl;
#endif // _DEBUG

	if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) // 调试断点，不处理
        return EXCEPTION_CONTINUE_SEARCH;
    LaunchReporter(ex);
    //TerminateProcess(GetCurrentProcess(), 0xDEAD0002);
    return EXCEPTION_CONTINUE_SEARCH;
}

// 栈溢出专用处理器（借助备用栈）
LONG CALLBACK StackOverflowHandler(EXCEPTION_POINTERS* ex)
{
#ifdef _DEBUG
    std::cout << "StackOverflowHandler called! Code: 0x" << std::hex << ex->ExceptionRecord->ExceptionCode << std::dec << std::endl;
#endif
    if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        LaunchReporter(ex);
        TerminateProcess(GetCurrentProcess(), 0xDEAD0002);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* ex)
{
#ifdef _DEBUG
	std::cout << "UEF called! Code: 0x" << std::hex << ex->ExceptionRecord->ExceptionCode << std::dec << std::endl;
#endif // _DEBUG
    LaunchReporter(ex);
    return EXCEPTION_EXECUTE_HANDLER;
}

void PureCallHandler()
{
#ifdef _DEBUG
	std::cout << "PureCallHandler called!" << std::endl;
#endif
    LaunchReporter(nullptr);
    abort();
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
#ifdef _DEBUG
	std::cout << "InvalidParameterHandler called!" << std::endl;
#endif
    LaunchReporter(nullptr);
    abort();
}

void TerminateHandler()
{
#ifdef _DEBUG
	std::cout << "TerminateHandler called!" << std::endl;
#endif
    EXCEPTION_RECORD rec = { STATUS_FATAL_APP_EXIT };
    CONTEXT ctx{};
	RtlCaptureContext(&ctx); // 捕获当前上下文信息，供 Reporter 分析调用栈等
    EXCEPTION_POINTERS ptr{ &rec, &ctx };
    LaunchReporter(&ptr);
    abort();
}

void SignalHandler(int)
{
#ifdef _DEBUG
	std::cout << "SignalHandler called!" << std::endl;
#endif
    LaunchReporter(nullptr);
    _exit(1);
}

LONG WINAPI MyFilter(EXCEPTION_POINTERS*)
{
    OutputDebugStringA("UEF called!\n");
    MessageBoxA(NULL, "UEF called", "Test", MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
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
    // 进程全局只做一次的操作（如创建互斥体、启动看门狗、注册 VEH）
	// 避免多次调用 Install 导致重复注册处理器、启动多个看门狗线程等问题
    if (!g_GlobalInitDone.exchange(true))
    {
        //if (!g_CrashMutex)
        //    g_CrashMutex = CreateMutexW(nullptr, FALSE, L"Global\\CAD_CrashMutex");  // 全局命名Mutex

        g_DumpLevel = level;
        g_ReporterPath = reporterPath.empty() ? L"CrashReporter.exe" : reporterPath;
        g_EnableWatchdog = enableWatchdog;

        // 预留栈溢出备用空间（64KB）
        //  用以发生栈溢出时，仍能在备用栈上执行异常处理器，生成 Dump
        ULONG guaranteeSize = 65536;
        SetThreadStackGuarantee(&guaranteeSize);

        // 安装向量化异常处理器（含栈溢出专用）
        //  VEH 的优先级高于 UEF，可以捕获更多异常（如栈溢出），并且不受当前模块的 SEH 影响
        //  VEH 链表累加，后安装的先执行，所以先安装通用处理器，再安装专门处理栈溢出的处理器，确保栈溢出时能正确处理
        AddVectoredExceptionHandler(1, VectoredHandler);
        AddVectoredExceptionHandler(1, StackOverflowHandler);

        // 启动看门狗线程（可选）
        //  看门狗线程定期检查主线程心跳，如果主线程长时间无响应（可能死循环或卡死），强制生成 Dump
        if (g_EnableWatchdog)
        {
            g_WatchdogThread = CreateThread(nullptr, 0, WatchdogProc, nullptr, 0, nullptr);
        }

        wprintf(L"[CrashHandler] Installed. Reporter: %ls, Level: %d, Watchdog: %s\n",
            g_ReporterPath.c_str(), (int)level, enableWatchdog ? "ON" : "OFF");
    }
    // 未处理异常过滤器
    //  UEF 的最后一道防线，捕获所有未被前面处理器捕获的异常
    //  UEF 单个指针覆盖
    // 每次调用都必须重新设置的 UEF（因为可能被其它代码覆盖）
    SetUnhandledExceptionFilter(TopLevelFilter);
    // C++ 运行时钩子
    //  捕获纯虚函数调用、无效参数、std::terminate 导致的崩溃等 C++ 运行时错误
    std::set_terminate(TerminateHandler);
    _set_purecall_handler(PureCallHandler);
    _set_invalid_parameter_handler(InvalidParameterHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // C 信号
    //  捕获 SIGABRT、SIGTERM 等信号引发的崩溃
    signal(SIGABRT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // 禁用 Windows 错误报告弹窗
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

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