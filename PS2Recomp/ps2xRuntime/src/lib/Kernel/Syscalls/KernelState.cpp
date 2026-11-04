// GOW-Port: save states (ps2_savestate.h): the EE kernel HLE's own tables outside the scheduler (SIF RPC clients and
// servers, boot mode, TLS, OSD config, SIF module records), plus the SIF and CD stubs' state (their own sources).
#include "Common.h"
#include "Helpers/State.h"
#include "runtime/ps2_state_io.h"

void ps2SifSaveState(ps2_state::Writer &w); // Stubs/SIF.cpp
bool ps2SifLoadState(ps2_state::Reader &r);
void ps2CdSaveState(ps2_state::Writer &w); // Stubs/CD.cpp
bool ps2CdLoadState(ps2_state::Reader &r);

namespace
{
    template <typename Map>
    std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> sorted(const Map &map)
    {
        std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> v(map.begin(), map.end());
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        return v;
    }
}

void ps2KernelSaveState(ps2_state::Writer &w)
{
    w.begin("KRNL");
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        w.pairs(sorted(g_rpc_servers));
        w.pairs(sorted(g_rpc_clients));
        w.boolean(g_rpc_initialized), w.u32(g_rpc_next_id), w.u32(g_rpc_packet_index), w.u32(g_rpc_server_index), w.u32(g_rpc_active_queue);
    }
    {
        std::lock_guard<std::mutex> lock(g_bootmode_mutex);
        w.boolean(g_bootmode_initialized), w.u32(g_bootmode_pool_offset);
        w.pairs(sorted(g_bootmode_addresses));
    }
    {
        std::lock_guard<std::mutex> lock(g_tls_mutex);
        w.u32(g_tls_index);
    }
    {
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        w.boolean(g_osd_config_initialized), w.u32(g_osd_config_raw), w.u32(g_osd_config2_raw);
    }
    {
        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        const auto modules = sorted(g_sif_modules_by_id);
        w.u32(static_cast<uint32_t>(modules.size()));
        for (const auto &[id, m] : modules)
            w.pod(m.id), w.string(m.path), w.string(m.pathKey), w.u32(m.refCount), w.boolean(m.loaded);
        w.u32(static_cast<uint32_t>(g_sif_module_id_by_path.size()));
        for (const auto &[path, id] : g_sif_module_id_by_path)
            w.string(path), w.pod(id);
        w.pod(g_next_sif_module_id);
    }
    ps2SifSaveState(w);
    ps2CdSaveState(w);
    w.end();
}

bool ps2KernelLoadState(ps2_state::Reader &r)
{
    if (!r.begin("KRNL"))
        return false;
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        const auto servers = r.pairs<uint32_t, RpcServerState>();
        const auto clients = r.pairs<uint32_t, RpcClientState>();
        g_rpc_servers = {servers.begin(), servers.end()};
        g_rpc_clients = {clients.begin(), clients.end()};
        g_rpc_initialized = r.boolean(), g_rpc_next_id = r.u32(), g_rpc_packet_index = r.u32(), g_rpc_server_index = r.u32();
        g_rpc_active_queue = r.u32();
    }
    {
        std::lock_guard<std::mutex> lock(g_bootmode_mutex);
        g_bootmode_initialized = r.boolean(), g_bootmode_pool_offset = r.u32();
        const auto addresses = r.pairs<uint8_t, uint32_t>();
        g_bootmode_addresses = {addresses.begin(), addresses.end()};
    }
    {
        std::lock_guard<std::mutex> lock(g_tls_mutex);
        g_tls_index = r.u32();
    }
    {
        std::lock_guard<std::mutex> lock(g_osd_mutex);
        g_osd_config_initialized = r.boolean(), g_osd_config_raw = r.u32(), g_osd_config2_raw = r.u32();
    }
    {
        std::lock_guard<std::mutex> lock(g_sif_module_mutex);
        g_sif_modules_by_id.clear();
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            SifModuleRecord m;
            r.pod(m.id);
            m.path = r.string(), m.pathKey = r.string(), m.refCount = r.u32(), m.loaded = r.boolean();
            const int32_t id = m.id;
            g_sif_modules_by_id.emplace(id, std::move(m));
        }
        g_sif_module_id_by_path.clear();
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            std::string path = r.string();
            int32_t id = 0;
            r.pod(id);
            g_sif_module_id_by_path.emplace(std::move(path), id);
        }
        r.pod(g_next_sif_module_id);
    }
    if (!ps2SifLoadState(r) || !ps2CdLoadState(r))
        return false;
    return r.end();
}
