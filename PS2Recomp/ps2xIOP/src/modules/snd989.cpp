#include "module_factories.h"
#include "rpc_reply.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Silent stand-in for Sony's 989snd sound driver (989nomid.irx, as used by
// God of War). The game's EE library zeroes the first and last words of each
// reply buffer and waits for the IOP to set both to -1 when a command has
// finished, so every command is acknowledged that way. Commands that return
// something (bank/stream handles) get a distinct nonzero value in between.
namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kSndMainSid = 0x00123456u;
        constexpr uint32_t kSndLoadSid = 0x00123457u;
        constexpr uint32_t kMaxLoggedCommands = 64u;

        class Snd989Service final : public IopService
        {
        public:
            explicit Snd989Service(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "989snd";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return kModuleAliases;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_commands = 0u;
                m_nextHandle = 0x00010000u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kSndMainSid && request.sid != kSndLoadSid)
                    return result;

                result.handled = true;
                result.resultAddress = request.receive.address;
                result.signalNowaitCompletion = true;
                result.signalCompletion = true;

                const size_t words = request.receive.size / sizeof(uint32_t);
                uint32_t handle = 0u;
                bool log = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    handle = m_nextHandle++;
                    log = m_commands++ < kMaxLoggedCommands;
                }
                if (words != 0u)
                {
                    std::vector<uint32_t> reply(words, handle);
                    reply.front() = 0xFFFFFFFFu;
                    reply.back() = 0xFFFFFFFFu;
                    (void)writeRpcWords(m_host, request.receive, reply);
                }
                if (log)
                {
                    std::ostringstream message;
                    message << "[989SND:HLE] sid=0x" << std::hex << request.sid
                            << " cmd=0x" << request.function
                            << " send=" << std::dec << request.send.size
                            << " recv=" << request.receive.size;
                    m_host.log(LogLevel::Info, message.str());
                }
                return result;
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"commands", m_commands, false});
            }

        private:
            inline static constexpr std::array<uint32_t, 2> kSids{kSndMainSid, kSndLoadSid};
            inline static constexpr std::array<std::string_view, 3> kModuleAliases{"989nomid", "989snd", "989snd_d"};

            IopHost &m_host;
            mutable std::mutex m_mutex;
            uint64_t m_commands = 0u;
            uint32_t m_nextHandle = 0x00010000u;
        };
    }

    std::unique_ptr<IopService> createSnd989Service(IopHost &host)
    {
        return std::make_unique<Snd989Service>(host);
    }
}
