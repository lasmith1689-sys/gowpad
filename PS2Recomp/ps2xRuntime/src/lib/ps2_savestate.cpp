// GOW-Port: save states (ps2_savestate.h). The pause menu asks on the host thread; the EE executor serves the request at
// its next safe point (EeScheduler::run, no guest code on the host stack), parked there while the menu is open.
//
// Slot file (savestates/slotN.state): a 64-byte header (magic, format version, build id, time, payload size) and the
// payload written by PS2Runtime::saveState: tagged sections (ps2_state_io.h). The build id ties a state to the exact
// recompiled code and runtime layout; a state from another build is listed but not loaded.
#include "runtime/ps2_savestate.h"
#include "runtime/ps2_state_io.h"
#include "ps2_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace
{
    constexpr char kMagic[8] = {'G', 'o', 'W', 'S', 'T', 'A', 'T', 'E'};
    constexpr uint32_t kFormat = 1;

    struct Header
    {
        char magic[8];
        uint32_t format;
        uint32_t headerSize;
        uint64_t build;
        int64_t savedAt; // seconds since 1970
        uint64_t payloadSize;
        uint8_t reserved[24];
    };
    static_assert(sizeof(Header) == 64);

    enum class Request
    {
        None,
        Save,
        Load,
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    std::atomic<bool> g_paused{false}, g_parked{false};
    std::atomic<bool> g_pending{false}; // paused or a request waits (read by the executor every dispatch)
    Request g_request = Request::None;
    int g_slot = 0;
    ps2_savestate::Status g_status = ps2_savestate::Status::Idle;
    std::string g_message;

    void updatePending()
    {
        g_pending.store(g_paused.load() || g_request != Request::None, std::memory_order_release);
    }

    void setStatus(ps2_savestate::Status status, std::string message)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status = status;
        g_message = std::move(message);
    }

    fs::path stateDir()
    {
#if defined(PS2X_IOS)
        const char *home = std::getenv("HOME");
        return fs::path(home ? home : ".") / "Documents" / "savestates";
#else
        const char *dir = std::getenv("GOW_STATE_DIR");
        return dir && *dir ? fs::path(dir) : fs::path("savestates");
#endif
    }

    std::string timeLabel(int64_t t)
    {
        const std::time_t tt = static_cast<std::time_t>(t);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char text[48];
        std::strftime(text, sizeof(text), "%b %d, %H:%M", &tm);
        return text;
    }

    bool readHeader(const fs::path &path, Header &h)
    {
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        if (!f)
            return false;
        const bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, kMagic, sizeof(kMagic)) == 0;
        std::fclose(f);
        return ok;
    }

    bool save(PS2Runtime &runtime, int slot, std::string &message)
    {
        const auto start = std::chrono::steady_clock::now();
        ps2_state::Writer w;
        if (!runtime.saveState(w, message))
            return false;
        std::error_code ec;
        fs::create_directories(stateDir(), ec);
        const fs::path path = ps2_savestate::slotPath(slot), temp = path.string() + ".tmp";
        Header h{};
        std::memcpy(h.magic, kMagic, sizeof(kMagic));
        h.format = kFormat;
        h.headerSize = sizeof(Header);
        h.build = runtime.stateBuildId();
        h.savedAt = static_cast<int64_t>(std::time(nullptr));
        h.payloadSize = w.data().size();
        std::FILE *f = std::fopen(temp.string().c_str(), "wb");
        if (!f)
        {
            message = "cannot write " + temp.string();
            return false;
        }
        bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1 && std::fwrite(w.data().data(), 1, w.data().size(), f) == w.data().size();
        ok = std::fclose(f) == 0 && ok;
        if (ok)
        {
            fs::rename(temp, path, ec); // (replaces the old state only once the new one is complete)
            ok = !ec;
        }
        if (!ok)
        {
            fs::remove(temp, ec);
            message = "write failed";
            return false;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[savestate] saved slot %d: %.1f MB in %.0f ms (%s)\n", slot + 1, w.data().size() / 1048576.0, ms, path.string().c_str());
        message = "Saved to slot " + std::to_string(slot + 1);
        return true;
    }

    bool load(PS2Runtime &runtime, int slot, std::string &message)
    {
        const auto start = std::chrono::steady_clock::now();
        const fs::path path = ps2_savestate::slotPath(slot);
        Header h{};
        if (!readHeader(path, h))
        {
            message = "slot " + std::to_string(slot + 1) + " is empty";
            return false;
        }
        if (h.format != kFormat || h.build != runtime.stateBuildId())
        {
            message = "slot " + std::to_string(slot + 1) + " is from another version";
            return false;
        }
        std::vector<uint8_t> payload(static_cast<size_t>(h.payloadSize));
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
        bool ok = f && std::fseek(f, static_cast<long>(h.headerSize), SEEK_SET) == 0 &&
                  std::fread(payload.data(), 1, payload.size(), f) == payload.size();
        if (f)
            std::fclose(f);
        if (!ok)
        {
            message = "slot " + std::to_string(slot + 1) + " is damaged";
            return false;
        }
        ps2_state::Reader r(payload.data(), payload.size());
        if (!runtime.loadState(r, message))
            return false;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(stderr, "[savestate] loaded slot %d in %.0f ms\n", slot + 1, ms);
        message = "Loaded slot " + std::to_string(slot + 1);
        return true;
    }
}

namespace ps2_savestate
{
    std::string slotPath(int slot)
    {
        return (stateDir() / ("slot" + std::to_string(slot + 1) + ".state")).string();
    }

    SlotInfo slotInfo(int slot)
    {
        SlotInfo info;
        Header h{};
        if (!readHeader(slotPath(slot), h))
            return info;
        info.exists = true;
        info.label = timeLabel(h.savedAt);
        info.loadable = h.format == kFormat && h.build == PS2Runtime::currentStateBuildId();
        if (!info.loadable)
            info.label += " (other version)";
        return info;
    }

    void setPaused(bool paused)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_paused.store(paused);
            updatePending();
        }
        g_cv.notify_all();
        if (paused)
            PS2Runtime::wakeExecutorForSafePoint();
    }

    bool parked() { return g_parked.load(); }

    void requestSave(int slot)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_request = Request::Save;
            g_slot = slot;
            g_status = Status::Working;
            g_message = "Saving...";
            updatePending();
        }
        g_cv.notify_all();
        PS2Runtime::wakeExecutorForSafePoint();
    }

    void requestLoad(int slot)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_request = Request::Load;
            g_slot = slot;
            g_status = Status::Working;
            g_message = "Loading...";
            updatePending();
        }
        g_cv.notify_all();
        PS2Runtime::wakeExecutorForSafePoint();
    }

    Status status(std::string *message)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (message)
            *message = g_message;
        return g_status;
    }

    void clearStatus()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status != Status::Working)
            g_status = Status::Idle, g_message.clear();
    }

    bool pending() noexcept { return g_pending.load(std::memory_order_acquire); }

    void serviceSafePoint(PS2Runtime &runtime)
    {
        // a save waits for a moment that can be saved (no system call half done, no movie, no file open): the game runs
        // on (behind the menu) and is stopped again at the first such moment, at most kSeekLimit later
        static bool seeking = false;
        static std::chrono::steady_clock::time_point seekStart{};
        constexpr auto kSeekLimit = std::chrono::seconds(5);
        const auto park = [&runtime](bool on) {
            if (g_parked.load() == on)
                return;
            g_parked.store(on);
            runtime.onSafePointPark(on);
        };
        for (;;)
        {
            Request request = Request::None;
            int slot = 0;
            bool paused = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                request = g_request;
                slot = g_slot;
                paused = g_paused.load();
            }
            if (request == Request::None && !paused)
                break;
            if (request == Request::Save)
            {
                std::string why;
                if (!runtime.stateSaveable(why))
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (!seeking)
                        seeking = true, seekStart = now;
                    if (now - seekStart < kSeekLimit)
                    {
                        park(false);
                        return; // (back here at the next safe point)
                    }
                    seeking = false;
                    std::fprintf(stderr, "[savestate] save slot %d failed: %s\n", slot + 1, why.c_str());
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_request = Request::None;
                    g_status = ps2_savestate::Status::Failed;
                    g_message = "Can't save right now (" + why + "), try again";
                    updatePending();
                    continue;
                }
                seeking = false;
            }
            if (request != Request::None)
            {
                park(true);
                std::string message;
                const bool ok = request == Request::Save ? save(runtime, slot, message) : load(runtime, slot, message);
                if (!ok)
                    std::fprintf(stderr, "[savestate] %s slot %d failed: %s\n", request == Request::Save ? "save" : "load", slot + 1, message.c_str());
                std::lock_guard<std::mutex> lock(g_mutex);
                g_request = Request::None;
                g_status = ok ? Status::Done : Status::Failed;
                g_message = message;
                updatePending();
                continue;
            }
            // paused, nothing asked: stopped here until the menu asks or closes
            park(true);
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_request != Request::None || !g_paused.load(); });
        }
        park(false);
        std::lock_guard<std::mutex> lock(g_mutex);
        updatePending();
    }
}
