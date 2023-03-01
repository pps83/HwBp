#include "HwBp.h"
#include <iostream>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <windows.h>

namespace
{
struct GlobalState
{
    static GlobalState& Get()
    {
        static GlobalState state;
        return state;
    }

    void Reset()
    {
        ResetBreakpoints();
    }

    void BreakpointHit()
    {
        ++breakPointFound;
    }

    int GetBp()
    {
        return breakPointFound;
    }

private:
    GlobalState()
    {
        ResetBreakpoints();
    }

    void ResetBreakpoints()
    {
        breakPointFound = 0;
    }

    int breakPointFound;
};

struct Test
{
    using TestFunc = void (*)();

    TestFunc func = nullptr;
    std::vector<std::ostringstream> output;
    bool success = true;
};

std::unordered_map<std::string, Test> g_tests;
Test* g_currentTest;

struct Registrar
{
    Registrar(const char* name, Test::TestFunc func)
    {
        g_tests[name].func = func;
    }
};

struct ExceptionHandler
{
    static constexpr ULONG c_callThisHandlerFirst = 1;

    ExceptionHandler(PVECTORED_EXCEPTION_HANDLER handler)
    {
        handle = ::AddVectoredExceptionHandler(c_callThisHandlerFirst, handler);
    }

    bool IsValid() const noexcept
    {
        return handle != nullptr;
    }

    ~ExceptionHandler()
    {
        if (handle)
        {
            ::RemoveVectoredExceptionHandler(handle);
        }
    }

    void* handle;
};

LONG MyHandler(_EXCEPTION_POINTERS* ExceptionInfo)
{
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP)
    {
        GlobalState::Get().BreakpointHit();
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

void AssertTrue(const char* str, bool result, int line, const char* file)
{
    if (!result)
    {
        g_currentTest->success = false;
        g_currentTest->output.emplace_back();
        g_currentTest->output.back() << "Assertion failed (" << file << ": " << line << "): " << str;
    }
}

#define ASSERT(Expr_) \
    AssertTrue(#Expr_, (Expr_), __LINE__, __FILE__);

#define REQUIRE(Expr_) \
    AssertTrue(#Expr_, (Expr_), __LINE__, __FILE__); \
    ReportResults(); \
    std::exit(EXIT_FAILURE);

#define TEST(Name_) \
    void Name_##TestFunc(); \
    Registrar Name_##Registrar{#Name_, Name_##TestFunc}; \
    void Name_##TestFunc() \

int ReportResults()
{
    int testsFailed = 0;
    for (auto&& t : g_tests)
    {
        if (!t.second.success)
        {
            std::cout << "Test '" << t.first << "' failed:\n";
            for (auto&& error : t.second.output)
            {
                std::cout << "    '" << error.str() << "'\n";
            }
            ++testsFailed;
        }
    }

    if (testsFailed == 0)
    {
        std::cout << "Success!\n";
        return EXIT_SUCCESS;
    }
    else
    {
        std::cout << testsFailed << " tests failed!\n";
        return EXIT_FAILURE;
    }
}
}

TEST(OneBp)
{
    GlobalState::Get().Reset();

    int var[1] = {42};
    auto bp = HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written);

    {
        ExceptionHandler h{ MyHandler };
        ASSERT(h.IsValid());

        var[0] = 33;

        ASSERT(GlobalState::Get().GetBp() == 1);
    }

    HwBp::Remove(bp);
}

TEST(FourBps)
{
    GlobalState::Get().Reset();

    int var[4] = { 42, 43, 44, 45 };
    HwBp::Breakpoint bps[4] = {
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written),
        HwBp::Set(&var[1], sizeof(var[1]), HwBp::When::Written),
        HwBp::Set(&var[2], sizeof(var[2]), HwBp::When::Written),
        HwBp::Set(&var[3], sizeof(var[3]), HwBp::When::Written)
    };

    {
        ExceptionHandler h{ MyHandler };

        var[0] = 30;
        ASSERT(bps[0].registerIndex == 0);
        ASSERT(bps[0].error == HwBp::Result::Success);
        ASSERT(GlobalState::Get().GetBp() == 1);

        var[1] = 31;
        ASSERT(bps[1].registerIndex == 1);
        ASSERT(GlobalState::Get().GetBp() == 2);

        var[2] = 32;
        ASSERT(bps[2].registerIndex == 2);
        ASSERT(GlobalState::Get().GetBp() == 3);

        var[3] = 33;
        ASSERT(bps[3].registerIndex == 3);
        ASSERT(GlobalState::Get().GetBp() == 4);
    }

    for (auto&& bp : bps)
    {
        HwBp::Remove(bp);
    }
}

TEST(FifthBpFails_ButWorksAfter3rdWasReleased)
{
    int var[1] = {0};
    const HwBp::Breakpoint bps[5] = {
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written),
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written),
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written),
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written),
        HwBp::Set(&var[0], sizeof(var[0]), HwBp::When::Written)
    };

    ASSERT(bps[4].error == HwBp::Result::NoAvailableRegisters);

    HwBp::Remove(bps[2]);

    auto bp5Retry = HwBp::Set(&var, sizeof(var), HwBp::When::Written);
    ASSERT(bp5Retry.error == HwBp::Result::Success);
    ASSERT(bp5Retry.registerIndex == bps[2].registerIndex);

    HwBp::Remove(bps[0]);
    HwBp::Remove(bps[1]);
    HwBp::Remove(bp5Retry);
    HwBp::Remove(bps[3]);
}

int main()
{
    for (auto&& t : g_tests)
    {
        g_currentTest = &t.second;
        t.second.func();
        g_currentTest = nullptr;
    }

    return ReportResults();
}
