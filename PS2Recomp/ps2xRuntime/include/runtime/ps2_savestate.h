// GOW-Port: save states (ps2_savestate.cpp): the whole emulated machine written to a slot file and read back later
// ("save anywhere"). The host thread (pause menu, ps2_pause_menu.cpp) asks; the EE executor does the work at a safe
// point between two guest dispatches (EeScheduler::run), where no guest code is on the host stack and every EE guest
// thread is a plain register context.
#pragma once
#include <cstdint>
#include <string>

class PS2Runtime;
namespace ps2_state
{
    class Writer;
    class Reader;
}

namespace ps2_savestate
{
    constexpr int kSlots = 3;

    struct SlotInfo
    {
        bool exists = false;
        std::string label; // when it was saved ("Nov 3, 21:40"), or why it cannot be loaded
        bool loadable = false;
    };
    SlotInfo slotInfo(int slot); // any thread: reads the slot file's header

    // host thread (pause menu)
    void setPaused(bool paused); // the emulation stops at its next safe point / goes on
    bool parked();               // the executor is stopped at a safe point
    void requestSave(int slot);
    void requestLoad(int slot);
    enum class Status
    {
        Idle,
        Working,
        Done,
        Failed,
    };
    Status status(std::string *message = nullptr);
    void clearStatus();

    // EE executor (EeScheduler::run, between guest dispatches)
    bool pending() noexcept; // cheap: a pause or a request is waiting
    void serviceSafePoint(PS2Runtime &runtime);

    // where the slot files go (Documents/savestates on iOS, ./savestates elsewhere)
    std::string slotPath(int slot);

    // state outside the machine that must travel with it (the runner's game overrides): saved after it, by tag
    using ExtraSave = void (*)(ps2_state::Writer &);
    using ExtraLoad = bool (*)(ps2_state::Reader &);
    void registerExtraState(const char *tag, ExtraSave save, ExtraLoad load);
}
