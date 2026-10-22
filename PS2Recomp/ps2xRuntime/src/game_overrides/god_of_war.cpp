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

    void applyGodOfWar(PS2Runtime &runtime)
    {
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
