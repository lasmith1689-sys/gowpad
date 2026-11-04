#pragma once

#include "ps2x/state_io.h"
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        explicit IopIoman(IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

        // GOW-Port: save states (ps2_savestate.h)
        void saveState(ps2_state::Writer &w) const;
        bool loadState(ps2_state::Reader &r);
    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        IopMemory &m_memory;
        std::vector<Device> m_devices;
    };
}
