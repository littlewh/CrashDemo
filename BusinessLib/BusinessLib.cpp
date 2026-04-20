#include "BusinessLib.h"
#include <stdexcept>
#include <vector>
#include <thread>

struct Base { virtual void foo() = 0; };
struct Derived : Base { void foo() override {} };

void BusinessLib::NullPointer()
{
    *(int*)nullptr = 0;
}

void BusinessLib::StackOverflow()
{
    volatile int buffer[1024] = { 0 };
    static int depth = 0;
    if (++depth > 10000)
        NullPointer();   // 确保触发异常
    StackOverflow();
}

void BusinessLib::PureVirtualCall()
{
    Base* p = nullptr;
    p->foo();
}

void BusinessLib::HeapCorruption()
{
    int* p = new int[10];
    delete[] p;
    p[5] = 0xDEADBEEF;   // use-after-free
}

void BusinessLib::UncaughtException()
{
    throw std::runtime_error("Test uncaught exception");
}

void BusinessLib::CrashInWorkerThread()
{
    /*std::thread t([] {
        NullPointer();
        });
    t.join();*/

    std::vector<std::thread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([] { BusinessLib::NullPointer(); });
    }
    for (auto& t : threads) if (t.joinable()) t.join();
}