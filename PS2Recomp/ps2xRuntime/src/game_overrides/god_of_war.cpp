// God of War (USA), SCUS-97399.
//
// The game allocates through its own sbrk (0x2942F0: a break pointer from the
// end of BSS at 0x35C1A8, capped by EndOfHeap), not through libc malloc, so
// the runtime's guest allocations (SIF packets, thread stacks, IOP host
// buffers) must not come from the same place: they overwrote the game's heap
// head at 0x35C1B0. Keep them in the 1 MB below the async-callback stacks.
#include "game_overrides.h"
#include "ps2_runtime.h"

namespace
{
    constexpr uint32_t kRuntimeHeapBase = 0x01E00000u;
    constexpr uint32_t kRuntimeHeapLimit = 0x01F00000u;

    void applyGodOfWar(PS2Runtime &runtime)
    {
        runtime.pinGuestHeap(kRuntimeHeapBase, kRuntimeHeapLimit);
    }
}

PS2_REGISTER_GAME_OVERRIDE("God of War (USA)", "SCUS_973.99", 0x00100008u, 0u, applyGodOfWar)
