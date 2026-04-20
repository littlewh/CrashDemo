#pragma once

class BusinessLib
{
public:
    static void NullPointer();          // 空指针
    static void StackOverflow();        // 栈溢出（递归）
    static void PureVirtualCall();      // 纯虚函数调用
    static void HeapCorruption();       // 堆损坏（use-after-free）
    static void UncaughtException();    // 未捕获异常 -> std::terminate
    static void CrashInWorkerThread();  // 工作线程中崩溃
};