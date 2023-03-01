#pragma once
#include <assert.h>
#include <stddef.h>
#include <algorithm>
#include <functional>
#include <array>
#include <bitset>
#include <windows.h>

namespace HwBp
{
enum class Result
{
    Success,
    CantGetThreadContext,
    CantSetThreadContext,
    NoAvailableRegisters,
    BadWhen, // Unsupported value of When passed
    BadSize // Size can only be 1, 2, 4, 8
};

enum class When
{
    ReadOrWritten,
    Written,
    Executed
};

struct Breakpoint
{
    static constexpr Breakpoint MakeFailed(Result result)
    {
        return { 0, result };
    }

    const uint8_t registerIndex;
    const Result error;
};

namespace impl
{
inline Breakpoint UpdateThreadContext(std::function<Breakpoint(CONTEXT& ctx, const std::array<bool, 4>& busyDebugRegister)> action)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (::GetThreadContext(::GetCurrentThread(), &ctx) == FALSE)
        return Breakpoint::MakeFailed(Result::CantGetThreadContext);
    std::array<bool, 4> busyDebugRegister{ {false, false, false, false} };
    auto checkBusyRegister = [&](size_t index, DWORD64 mask)
    {
        if (ctx.Dr7 & mask)
            busyDebugRegister[index] = true;
    };
    checkBusyRegister(0, 1);
    checkBusyRegister(1, 4);
    checkBusyRegister(2, 16);
    checkBusyRegister(3, 64);
    const auto actionResult = action(ctx, busyDebugRegister);
    if (::SetThreadContext(::GetCurrentThread(), &ctx) == FALSE)
        return Breakpoint::MakeFailed(Result::CantSetThreadContext);
    return actionResult;
}
} // namespace impl

inline Breakpoint Set(const void* onPointer, uint8_t size, When when)
{
    return impl::UpdateThreadContext(
        [onPointer, size, when](CONTEXT& ctx, const std::array<bool, 4>& busyDebugRegister) -> Breakpoint
        {
            const auto found = std::find(std::begin(busyDebugRegister), std::end(busyDebugRegister), false);
            if (found == std::end(busyDebugRegister))
                return Breakpoint::MakeFailed(Result::NoAvailableRegisters);
            const auto registerIndex = static_cast<uint16_t>(std::distance(std::begin(busyDebugRegister), found));
            switch (registerIndex)
            {
            case 0:
                ctx.Dr0 = reinterpret_cast<DWORD_PTR>(const_cast<void*>(onPointer));
                break;
            case 1:
                ctx.Dr1 = reinterpret_cast<DWORD_PTR>(const_cast<void*>(onPointer));
                break;
            case 2:
                ctx.Dr2 = reinterpret_cast<DWORD_PTR>(const_cast<void*>(onPointer));
                break;
            case 3:
                ctx.Dr3 = reinterpret_cast<DWORD_PTR>(const_cast<void*>(onPointer));
                break;
            default:
                assert(!"Impossible happened - searching in array of 4 got index < 0 or > 3");
                exit(EXIT_FAILURE);
            }
            std::bitset<sizeof(ctx.Dr7) * 8> dr7;
            memcpy(&dr7, &ctx.Dr7, sizeof(ctx.Dr7));
            dr7.set(registerIndex * 2); // Flag to enable 'local' debugging for each of 4 registers. Second bit is for global debugging, not working.
            switch (when)
            {
            case When::ReadOrWritten:
                dr7.set(16 + registerIndex * 4 + 1, true);
                dr7.set(16 + registerIndex * 4, true);
                break;
            case When::Written:
                dr7.set(16 + registerIndex * 4 + 1, false);
                dr7.set(16 + registerIndex * 4, true);
                break;
            case When::Executed:
                dr7.set(16 + registerIndex * 4 + 1, false);
                dr7.set(16 + registerIndex * 4, false);
                break;
            default:
                return Breakpoint::MakeFailed(Result::BadWhen);
            }
            switch (size)
            {
            case 1:
                dr7.set(16 + registerIndex * 4 + 3, false);
                dr7.set(16 + registerIndex * 4 + 2, false);
                break;
            case 2:
                dr7.set(16 + registerIndex * 4 + 3, false);
                dr7.set(16 + registerIndex * 4 + 2, true);
                break;
            case 8:
                dr7.set(16 + registerIndex * 4 + 3, true);
                dr7.set(16 + registerIndex * 4 + 2, false);
                break;
            case 4:
                dr7.set(16 + registerIndex * 4 + 3, true);
                dr7.set(16 + registerIndex * 4 + 2, true);
                break;
            default:
                return Breakpoint::MakeFailed(Result::BadSize);
            }
            memcpy(&ctx.Dr7, &dr7, sizeof(ctx.Dr7));
            return Breakpoint{ static_cast<uint8_t>(registerIndex), Result::Success };
        });
}

inline void Remove(const Breakpoint& bp)
{
    if (bp.error != Result::Success)
        return;
    impl::UpdateThreadContext(
        [&bp](CONTEXT& ctx, const std::array<bool, 4>&) -> Breakpoint
        {
            std::bitset<sizeof(ctx.Dr7) * 8> dr7;
            memcpy(&dr7, &ctx.Dr7, sizeof(ctx.Dr7));
            dr7.set(bp.registerIndex * 2, false); // Flag to enable 'local' debugging for each of 4 registers. Second bit is for global debugging, not working.
            memcpy(&ctx.Dr7, &dr7, sizeof(ctx.Dr7));
            return Breakpoint{};
        });
}
} // namespace HwBp
