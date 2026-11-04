// GOW-Port: save states, the machine as a whole (ps2_savestate.h). Taken on the EE executor between two guest dispatches
// (EeScheduler::run): every EE guest thread is a register context, the IOP is between batches (it runs from the EE's
// cycle accounting), the GS worker, stage and GL threads are drained by the sections that need them. Sections, in
// order: EE memory and registers, GIF arbiter, VU0/VU1, GS (renderer, then GS memory and registers), EE scheduler,
// the runtime's own tables, the EE kernel HLE tables, the IOP, then the registered extras (the game's overrides).
#include "ps2_runtime.h"
#include "ps2_iop_host.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_savestate.h"
#include "runtime/ps2_state_io.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

void ps2KernelSaveState(ps2_state::Writer &w); // Kernel/Syscalls/KernelState.cpp
bool ps2KernelLoadState(ps2_state::Reader &r);
bool ps2MpegPlaying(); // Kernel/Stubs/MPEG.cpp

namespace
{
    struct Extra
    {
        std::string tag;
        ps2_savestate::ExtraSave save;
        ps2_savestate::ExtraLoad load;
    };
    std::vector<Extra> &extras()
    {
        static std::vector<Extra> list;
        return list;
    }
    std::chrono::steady_clock::time_point g_parkStart{};

    // bump when anything saved changes shape (a field added to a saved structure, a section reordered)
    constexpr uint64_t kStateLayout = 1;
}

namespace ps2_savestate
{
    void registerExtraState(const char *tag, ExtraSave save, ExtraLoad load)
    {
        for (const Extra &e : extras())
            if (e.tag == tag)
                return;
        extras().push_back({tag, save, load});
        std::sort(extras().begin(), extras().end(), [](const Extra &a, const Extra &b) { return a.tag < b.tag; });
    }
}

uint64_t PS2Runtime::currentStateBuildId()
{
    static const uint64_t id = []
    {
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(kStateLayout);
        // the plain structures written as they are
        mix(sizeof(R5900Context)), mix(sizeof(VU1State)), mix(sizeof(GSContext)), mix(sizeof(GSVertex)), mix(sizeof(EeAlarm));
        mix(sizeof(EeIrqHandler)), mix(sizeof(EeEvent)), mix(sizeof(VIFRegisters)), mix(sizeof(DMARegisters));
        // the recompiled code: where guest functions (resume points) are
        mix(g_ps2RecompiledFunctionTableBase), mix(g_ps2RecompiledFunctionTableEnd), mix(g_ps2RecompiledFunctionTableSlotCount);
        for (uint32_t i = 0; i < g_ps2RecompiledFunctionTableSlotCount; ++i)
            if (g_ps2RecompiledFunctionTable[i])
                mix(i);
        return h;
    }();
    return id;
}

void PS2Runtime::wakeExecutorForSafePoint()
{
    // (the executor reaches a safe point within a field: VBlank deadlines wake it when it is idle)
}

void PS2Runtime::onSafePointPark(bool parked)
{
    const auto now = std::chrono::steady_clock::now();
    if (parked)
    {
        g_parkStart = now;
        return;
    }
    // the time stopped does not count: the scheduled fields move by it, the VBlank pacing starts again
    if (m_eeScheduler && g_parkStart != std::chrono::steady_clock::time_point{})
    {
        m_eeScheduler->shiftHostDeadlines(now - g_parkStart);
        m_eeScheduler->resyncHostPacing();
    }
    g_parkStart = {};
}

bool PS2Runtime::stateSaveable(std::string &why)
{
    if (!m_eeScheduler || !m_iopSubsystem)
    {
        why = "the game is not running";
        return false;
    }
    if (!m_eeScheduler->canSaveState(why))
        return false;
    if (!m_iopSubsystem->quiescent())
    {
        why = "the sound processor is busy";
        return false;
    }
    if (ps2MpegPlaying())
    {
        why = "a movie is playing";
        return false;
    }
    if (!m_vfs.descriptors().empty() || (m_iopHost && m_iopHost->openHostFileCount() != 0u))
    {
        why = "the game has files open";
        return false;
    }
    return true;
}

bool PS2Runtime::saveState(ps2_state::Writer &w, std::string &message)
{
    std::string why;
    if (!stateSaveable(why))
    {
        message = "Can't save now: " + why;
        return false;
    }
    w.begin("RUNT");
    m_memory.saveState(w); // (drains the GS worker and stage first)
    m_gifArbiter.saveState(w);
    m_vu0.saveState(w);
    m_vu1.saveState(w);
    if (!m_gs.saveState(w))
    {
        message = "the graphics state could not be read";
        return false;
    }
    // (parked: the guest stopped when the menu opened; the deadlines are kept relative to that moment)
    m_eeScheduler->saveState(w, g_parkStart != std::chrono::steady_clock::time_point{} ? g_parkStart : std::chrono::steady_clock::now());

    w.begin("RTEE");
    w.pod(m_cpuContext);
    {
        std::lock_guard lock(m_eeKernelStateMutex);
        std::vector<int> threads;
        for (const auto &kv : m_eeExitHandlers)
            threads.push_back(kv.first);
        std::sort(threads.begin(), threads.end());
        w.u32(static_cast<uint32_t>(threads.size()));
        for (const int id : threads)
            w.pod(id), w.vector(m_eeExitHandlers.at(id));
        std::vector<std::pair<uint32_t, uint32_t>> overrides(m_eeSyscallOverrides.begin(), m_eeSyscallOverrides.end());
        std::sort(overrides.begin(), overrides.end());
        w.pairs(overrides);
        std::vector<uint32_t> mirrors(m_eeSyscallMirrorAddresses.begin(), m_eeSyscallMirrorAddresses.end());
        std::sort(mirrors.begin(), mirrors.end());
        w.vector(mirrors);
    }
    {
        std::lock_guard lock(m_guestHeapMutex);
        w.vector(m_guestHeapBlocks);
        w.u32(m_guestHeapBase), w.u32(m_guestHeapEnd), w.u32(m_guestHeapLimit), w.u32(m_guestHeapSuggestedBase);
        w.boolean(m_guestHeapConfigured), w.u32(m_privateGuestHeapBase), w.u32(m_privateGuestHeapLimit);
    }
    {
        std::lock_guard lock(m_asyncCallbackStackMutex);
        w.u32(m_asyncCallbackStackFloor), w.u32(m_asyncCallbackStackTop);
    }
    w.u64(m_eeCheckpointSerial);
    w.u32(static_cast<uint32_t>(m_loadedModules.size()));
    for (const LoadedModule &m : m_loadedModules)
        w.string(m.name), w.u32(m.baseAddress), w.u64(m.size), w.boolean(m.active);
    w.end();

    ps2KernelSaveState(w);
    m_iopSubsystem->saveState(w);
    w.u32(static_cast<uint32_t>(extras().size()));
    for (const Extra &e : extras())
    {
        w.string(e.tag);
        e.save(w);
    }
    w.end();
    return true;
}

bool PS2Runtime::loadState(ps2_state::Reader &r, std::string &message)
{
    // (a damaged or foreign state fails in its first sections, before the IOP and the kernel tables are touched; the
    // EE memory and GS are already replaced then, so a failure past them leaves the game unusable: said so)
    const auto fail = [&](const char *what) {
        message = std::string("Load failed (") + what + (r.error().empty() ? "" : ": " + r.error()) + "): restart the game";
        return false;
    };
    if (!r.begin("RUNT"))
        return fail("header");
    if (!m_memory.loadState(r))
        return fail("memory");
    if (!m_gifArbiter.loadState(r))
        return fail("GIF");
    if (!m_vu0.loadState(r) || !m_vu1.loadState(r))
        return fail("VU");
    if (!m_gs.loadState(r))
        return fail("graphics");
    if (!m_eeScheduler->loadState(r))
        return fail("scheduler");

    if (!r.begin("RTEE"))
        return fail("runtime");
    r.pod(m_cpuContext);
    {
        std::lock_guard lock(m_eeKernelStateMutex);
        m_eeExitHandlers.clear();
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            int id = 0;
            r.pod(id);
            m_eeExitHandlers[id] = r.vector<EeExitHandlerRegistration>();
        }
        const auto overrides = r.pairs<uint32_t, uint32_t>();
        m_eeSyscallOverrides = {overrides.begin(), overrides.end()};
        const auto mirrors = r.vector<uint32_t>();
        m_eeSyscallMirrorAddresses = {mirrors.begin(), mirrors.end()};
    }
    {
        std::lock_guard lock(m_guestHeapMutex);
        m_guestHeapBlocks = r.vector<GuestHeapBlock>();
        m_guestHeapBase = r.u32(), m_guestHeapEnd = r.u32(), m_guestHeapLimit = r.u32(), m_guestHeapSuggestedBase = r.u32();
        m_guestHeapConfigured = r.boolean(), m_privateGuestHeapBase = r.u32(), m_privateGuestHeapLimit = r.u32();
    }
    {
        std::lock_guard lock(m_asyncCallbackStackMutex);
        m_asyncCallbackStackFloor = r.u32(), m_asyncCallbackStackTop = r.u32();
    }
    m_eeCheckpointSerial = r.u64();
    std::vector<LoadedModule> modules;
    for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
    {
        LoadedModule m{};
        m.name = r.string(), m.baseAddress = r.u32(), m.size = static_cast<size_t>(r.u64()), m.active = r.boolean();
        modules.push_back(std::move(m));
    }
    m_loadedModules = std::move(modules);
    if (!r.end())
        return fail("runtime");

    if (!ps2KernelLoadState(r))
        return fail("kernel");
    if (!m_iopSubsystem->loadState(r))
        return fail("IOP");
    for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
    {
        const std::string tag = r.string();
        const auto it = std::find_if(extras().begin(), extras().end(), [&](const Extra &e) { return e.tag == tag; });
        if (it == extras().end() || !it->load(r))
            return fail("game data");
    }
    if (!r.end())
        return fail("end");
    // the time since the menu opened is before the loaded moment: nothing to make up for
    g_parkStart = std::chrono::steady_clock::now();
    m_eeScheduler->resyncHostPacing();
    return true;
}
