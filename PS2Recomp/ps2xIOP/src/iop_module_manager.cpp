#include "ps2x/state_io.h"
#include "iop_module_manager.h"

#include "ps2x/iop/ps2_path.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    IopModuleManager::IopModuleManager()
    {
        // ROM modules that the no-BIOS HLE environment can legitimately provide. 
        // Entries with RPC services become routable only after load.
        constexpr std::string_view modules[] = {
            "sysmem",
            "loadcore",
            "intrman",
            "sifman",
            "sifcmd",
            "sifinit",
            "ioman",
            "iomanx",
            "modload",
            "stdio",
            "sysclib",
            "thbase",
            "thevent",
            "thsemap",
            "thmsgbx",
            "timrman",
            "vblank",
            "secrman",
            "sio2man",
            "xsio2man",
            "sio2d",
            "padman",
            "xpadman",
            "mcman",
            "xmcman",
            "mcserv",
            "libsd",
            "cdvdman",
            "cdvdfsv",
            "dev9",
            "usbd",
            "usbhdfsd",
            "udnl",
            "fileio",
            "poweroff",
            "netman",
            "ps2ip",
            "dbcman",
            "dbcm",
        };
        for (const std::string_view module : modules)
            m_builtinKeys.emplace(module);
    }

    void IopModuleManager::reset()
    {
        m_records.clear();
        m_hleIdsByKey.clear();
        m_loadedKeyReferences.clear();
        m_nextHleId = 0x40000000;
    }

    void IopModuleManager::setServiceModuleKeys(std::vector<std::string> keys)
    {
        m_serviceKeys.clear();
        for (std::string &key : keys)
        {
            const std::string normalized = ps2PathLeafKey(key);
            if (!normalized.empty())
                m_serviceKeys.emplace(normalized);
        }
    }

    ModuleLoadResult IopModuleManager::loadHle(std::string_view path)
    {
        ModuleLoadResult result{true, -1, -1};
        const std::string key = ps2PathLeafKey(path);
        if (key.empty() || (!m_builtinKeys.contains(key) && !m_serviceKeys.contains(key)))
            return result;

        const auto existing = m_hleIdsByKey.find(key);
        if (existing != m_hleIdsByKey.end())
        {
            Record &record = m_records[existing->second];
            ++record.references;
            addLoadedKey(key);
            result.moduleId = existing->second;
            result.startResult = 0;
            return result;
        }

        if (m_nextHleId <= 0)
            return result;
        const int32_t id = m_nextHleId++;
        m_records.emplace(id, Record{key, 1u, false});
        m_hleIdsByKey.emplace(key, id);
        addLoadedKey(key);
        result.moduleId = id;
        result.startResult = 0;
        return result;
    }

    void IopModuleManager::observePhysicalLoad(int32_t moduleId, std::string_view path)
    {
        if (moduleId <= 0)
            return;
        const std::string key = ps2PathLeafKey(path);
        if (key.empty())
            return;
        m_records[moduleId] = Record{key, 1u, true};
        addLoadedKey(key);
    }

    bool IopModuleManager::stopHle(int32_t moduleId, int32_t *result)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || found->second.physical)
            return false;

        Record &record = found->second;
        removeLoadedKey(record.key);
        if (record.references > 1u)
        {
            --record.references;
        }
        else
        {
            m_hleIdsByKey.erase(record.key);
            m_records.erase(found);
        }
        if (result)
            *result = 0;
        return true;
    }

    void IopModuleManager::observePhysicalStop(int32_t moduleId)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || !found->second.physical)
            return;
        removeLoadedKey(found->second.key);
        m_records.erase(found);
    }

    bool IopModuleManager::isLoaded(std::span<const std::string_view> aliases) const
    {
        if (aliases.empty())
            return true;
        return std::any_of(aliases.begin(), aliases.end(), [&](std::string_view alias)
                           {
                               const std::string key = ps2PathLeafKey(alias);
                               const auto found = m_loadedKeyReferences.find(key);
                               return found != m_loadedKeyReferences.end() && found->second != 0u; });
    }

    bool IopModuleManager::recognizes(std::string_view path) const
    {
        const std::string key = ps2PathLeafKey(path);
        return m_builtinKeys.contains(key) || m_serviceKeys.contains(key);
    }

    void IopModuleManager::addLoadedKey(std::string_view key)
    {
        ++m_loadedKeyReferences[std::string(key)];
    }

    void IopModuleManager::removeLoadedKey(std::string_view key)
    {
        const auto found = m_loadedKeyReferences.find(std::string(key));
        if (found == m_loadedKeyReferences.end())
            return;
        if (found->second > 1u)
            --found->second;
        else
            m_loadedKeyReferences.erase(found);
    }
}

namespace ps2x::iop::detail
{
    void IopModuleManager::saveState(ps2_state::Writer &w) const
    {
        w.begin("IMOD");
        std::vector<std::pair<int32_t, const Record *>> records;
        for (const auto &kv : m_records)
            records.emplace_back(kv.first, &kv.second);
        std::sort(records.begin(), records.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        w.u32(static_cast<uint32_t>(records.size()));
        for (const auto &[id, rec] : records)
            w.pod(id), w.string(rec->key), w.u32(rec->references), w.boolean(rec->physical);
        w.u32(static_cast<uint32_t>(m_hleIdsByKey.size()));
        for (const auto &kv : m_hleIdsByKey)
            w.string(kv.first), w.pod(kv.second);
        w.u32(static_cast<uint32_t>(m_loadedKeyReferences.size()));
        for (const auto &kv : m_loadedKeyReferences)
            w.string(kv.first), w.u32(kv.second);
        w.pod(m_nextHleId);
        w.end();
    }

    bool IopModuleManager::loadState(ps2_state::Reader &r)
    {
        if (!r.begin("IMOD"))
            return false;
        std::unordered_map<int32_t, Record> records;
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            int32_t id = 0;
            r.pod(id);
            Record rec;
            rec.key = r.string(), rec.references = r.u32(), rec.physical = r.boolean();
            records.emplace(id, std::move(rec));
        }
        std::unordered_map<std::string, int32_t> ids;
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            std::string key = r.string();
            int32_t id = 0;
            r.pod(id);
            ids.emplace(std::move(key), id);
        }
        std::unordered_map<std::string, uint32_t> refs;
        for (uint32_t i = 0, n = r.u32(); i < n && r.ok(); ++i)
        {
            std::string key = r.string();
            refs.emplace(std::move(key), r.u32());
        }
        int32_t next = 0;
        r.pod(next);
        if (!r.end())
            return false;
        m_records = std::move(records);
        m_hleIdsByKey = std::move(ids);
        m_loadedKeyReferences = std::move(refs);
        m_nextHleId = next;
        return true;
    }
}
