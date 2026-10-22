// God of War (USA), SCUS-97399.
//
// The game owns all of user RAM: its sbrk heap runs from the end of BSS
// (0x35C1A8) to 0x1FF7FF8, allocated top-down, and its main thread stack is
// the 32 KB above that. The runtime's own guest allocations (SIF packets,
// thread stacks, IOP host buffers) and the stacks it runs interrupt handlers
// on default to the top of RAM, where they overwrote the game's heap and the
// main thread's live stack frames. Put both in the EE kernel area instead,
// which a real game never touches and the runtime's HLE kernel leaves empty.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace
{
    constexpr uint32_t kRuntimeHeapBase = 0x00010000u;
    constexpr uint32_t kRuntimeHeapLimit = 0x00048000u;
    constexpr uint32_t kCallbackStackFloor = 0x00048000u;
    constexpr uint32_t kCallbackStackTop = 0x00080000u;

    // Debug aid (PS2X_GOW_TRACE_ALLOC): log the game's heap allocator, 0x13DE28(heap, bytes, align).
    constexpr uint32_t kHeapAlloc = 0x0013DE28u;
    PS2Runtime::RecompiledFunction g_heapAlloc = nullptr;

    void tracedHeapAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t heap = getRegU32(ctx, 4);
        const uint32_t bytes = getRegU32(ctx, 5);
        const uint32_t align = getRegU32(ctx, 6);
        const uint32_t ra = getRegU32(ctx, 31);
        g_heapAlloc(rdram, ctx, runtime);
        std::fprintf(stderr, "[gow alloc] heap=0x%x bytes=0x%x align=0x%x ra=0x%x -> 0x%x\n",
                     heap, bytes, align, ra, getRegU32(ctx, 2));
    }

    // Leaf routines reached only through function pointers that the analyzer left in gaps
    // between functions (the recompiler emits nothing for them).
    void gowSetHandlerSlot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        // 0x1738D8: dsll32/dsra32 a1; sb zero,0x19(a1); sw a0,0x20(a1); jr ra
        const uint32_t object = getRegU32(ctx, 5);
        const uint32_t value = getRegU32(ctx, 4);
        SET_GPR_S32(ctx, 5, static_cast<int32_t>(object));
        rdram[(object + 0x19u) & PS2_RAM_MASK] = 0u;
        std::memcpy(rdram + ((object + 0x20u) & PS2_RAM_MASK), &value, sizeof(value));
        ctx->pc = getRegU32(ctx, 31);
    }

    void gowReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        // 0x131AB0: jr ra
        ctx->pc = getRegU32(ctx, 31);
    }

    // libpad2 over libdbc talks to dbcman.irx, which drives SIO2 hardware the IOP
    // emulator does not have. Answer the game's libpad2 calls from the host pad instead:
    // one DUALSHOCK 2 on socket 0 (18-byte reads: 2 active-low button bytes, rx ry lx ly,
    // 12 pressures R L U D Tri Cir X Sq L1 R1 L2 R2).
    constexpr uint32_t kPad2CreateSocket = 0x0027B890u;
    constexpr uint32_t kPad2Read = 0x0027B9F0u;
    constexpr uint32_t kPad2GetButtonProfile = 0x0027BAC8u;
    constexpr uint32_t kPad2GetState = 0x0027BB98u;
    constexpr uint32_t kPad2GetActuatorCount = 0x0027BF90u;
    constexpr uint8_t kDualShock2Profile[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};

    void returnS32(R5900Context *ctx, int32_t value)
    {
        setReturnS32(ctx, value);
        ctx->pc = getRegU32(ctx, 31);
    }

    void gowPad2CreateSocket(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnS32(ctx, 0); }
    void gowPad2GetState(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnS32(ctx, 1); }
    void gowPad2GetActuatorCount(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnS32(ctx, 0); }

    void gowPad2GetButtonProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t out = getRegU32(ctx, 5);
        std::memcpy(rdram + (out & PS2_RAM_MASK), kDualShock2Profile, sizeof(kDualShock2Profile));
        returnS32(ctx, static_cast<int32_t>(sizeof(kDualShock2Profile)));
    }

    void gowPad2Read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint8_t raw[32] = {};
        if (!runtime->padBackend().readState(0, 0, raw, sizeof(raw)))
        {
            raw[2] = raw[3] = 0xFFu;
            raw[4] = raw[5] = raw[6] = raw[7] = 0x80u;
        }
        uint8_t data[18] = {};
        std::memcpy(data, raw + 2, 6); // buttons, rx, ry, lx, ly
        const uint16_t pressed = static_cast<uint16_t>(~(raw[2] | (raw[3] << 8)));
        // pressure order -> libpad button bit (low byte: Select L3 R3 Start Up Right Down Left)
        constexpr uint16_t kPressureBits[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
                                                0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
        for (int i = 0; i < 12; ++i)
            data[6 + i] = (pressed & kPressureBits[i]) ? 0xFFu : 0x00u;
        const uint32_t out = getRegU32(ctx, 5);
        std::memcpy(rdram + (out & PS2_RAM_MASK), data, sizeof(data));
        returnS32(ctx, static_cast<int32_t>(sizeof(data)));
    }

    // Debug aid (PS2X_GOW_TRACE_FNS=0xADDR,0xADDR,...): log sp/ra/a0-a3 around calls to
    // those guest functions (up to 8), for the first PS2X_GOW_TRACE_LIMIT (default 200) calls each.
    constexpr size_t kMaxTracedFns = 8;
    uint32_t g_tracedAddrs[kMaxTracedFns] = {};
    PS2Runtime::RecompiledFunction g_tracedOriginals[kMaxTracedFns] = {};
    uint32_t g_tracedCalls[kMaxTracedFns] = {};

    template <size_t I>
    void tracedFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const uint32_t s_limit = []
        {
            const char *value = std::getenv("PS2X_GOW_TRACE_LIMIT");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 200u;
        }();
        const bool log = g_tracedCalls[I]++ < s_limit;
        const uint32_t sp = getRegU32(ctx, 29);
        if (log)
            std::fprintf(stderr, "[gow fn] enter 0x%x sp=0x%x ra=0x%x a0=0x%x a1=0x%x a2=0x%x a3=0x%x\n",
                         g_tracedAddrs[I], sp, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 7));
        g_tracedOriginals[I](rdram, ctx, runtime);
        if (log)
            std::fprintf(stderr, "[gow fn] leave 0x%x sp=0x%x (entry 0x%x) pc=0x%x v0=0x%x\n",
                         g_tracedAddrs[I], getRegU32(ctx, 29), sp, ctx->pc, getRegU32(ctx, 2));
    }

    template <size_t... I>
    void installFunctionTraces(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        const char *list = std::getenv("PS2X_GOW_TRACE_FNS");
        if (!list)
            return;
        constexpr PS2Runtime::RecompiledFunction wrappers[] = {tracedFunction<I>...};
        size_t count = 0;
        for (const char *p = list; *p && count < kMaxTracedFns;)
        {
            char *end = nullptr;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(p, &end, 0));
            if (end == p)
                break;
            g_tracedAddrs[count] = address;
            g_tracedOriginals[count] = runtime.lookupFunction(address);
            runtime.registerFunction(address, wrappers[count]);
            ++count;
            p = (*end == ',') ? end + 1 : end;
        }
    }

    void applyGodOfWar(PS2Runtime &runtime)
    {
        installFunctionTraces(runtime, std::make_index_sequence<kMaxTracedFns>{});
        runtime.registerFunction(kPad2Read, gowPad2Read);
        runtime.registerFunction(kPad2GetButtonProfile, gowPad2GetButtonProfile);
        runtime.registerFunction(kPad2GetState, gowPad2GetState);
        runtime.registerFunction(kPad2GetActuatorCount, gowPad2GetActuatorCount);
        runtime.registerFunction(0x001738D8u, gowSetHandlerSlot);
        runtime.registerFunction(0x00131AB0u, gowReturn);
        runtime.pinGuestHeap(kRuntimeHeapBase, kRuntimeHeapLimit);
        runtime.pinAsyncCallbackStacks(kCallbackStackFloor, kCallbackStackTop);
        if (std::getenv("PS2X_GOW_TRACE_ALLOC"))
        {
            g_heapAlloc = runtime.lookupFunction(kHeapAlloc);
            runtime.registerFunction(kHeapAlloc, tracedHeapAlloc);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("God of War (USA)", "SCUS_973.99", 0x00100008u, 0u, applyGodOfWar)
