#ifndef PS2_PERF_H
#define PS2_PERF_H

// GOW-Port: perfil opcional de tiempo transcurrido exclusivo, separado por hilo.
// Los tiempos incluyen esperas del sistema; no representan muestras de uso de CPU.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>
#include <algorithm>

namespace ps2_perf
{
    // GsQueueWait: the GS frontend blocked on a full GL-thread queue (or a drain); counted apart from Gs so the log shows
    // whether the frontend or the GL thread behind it is the limit
    enum class Bucket : size_t { Ee, Vu, Gs, Iop, GuestWait, Upload, HostWait, GsQueueWait, Count, None = Count };
    constexpr size_t bucketCount = static_cast<size_t>(Bucket::Count);

    // Contabilidad con reloj inyectable para comprobar el anidamiento sin sleeps.
    struct Accounting
    {
        Bucket active = Bucket::None;
        uint64_t since = 0;

        struct Charge { Bucket bucket; uint64_t nanos; };
        Charge switchTo(Bucket next, uint64_t now)
        {
            const Charge charge{active, now >= since ? now - since : 0u};
            active = next;
            since = now;
            return charge;
        }
    };

    inline bool enabled()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_PERF_DIAG");
            return value && std::strcmp(value, "1") == 0;
        }();
        return on;
    }
    inline bool countFrames()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_PERF_DIAG");
            return value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "frames") == 0);
        }();
        return on;
    }
    inline std::array<std::atomic<uint64_t>, bucketCount> nanos{};
    inline std::atomic<uint64_t> guestFlips{0}, hostFrames{0};
    // GOW-Port: the latest report's values, for an on-screen readout (iOS has no console): game frames and host frames
    // per second, game speed (GOW_DELTA_DIAG), and each bucket's share of one core in percent
    struct Latest
    {
        std::atomic<float> guestHz{0.0f}, hostHz{0.0f}, speed{0.0f};
        std::array<std::atomic<float>, bucketCount> corePercent{};
        std::atomic<float> gpuFramesBehind{0.0f}; // presented frames the GPU had not finished, averaged (GS backend)
        std::mutex glLineMutex;
        std::string glLine; // the GL thread's time split ([gs-gl], gs_threaded_backend.cpp)
    };
    inline Latest latest;
    inline void setGlLine(std::string line)
    {
        std::lock_guard<std::mutex> lock(latest.glLineMutex);
        latest.glLine = std::move(line);
    }
    inline std::string glLine()
    {
        std::lock_guard<std::mutex> lock(latest.glLineMutex);
        return latest.glLine;
    }

    // GOW-Port: hitch readout (iOS, on screen): the longest game frame and the worst single GS GL-thread event of the
    // last 30 s, each kept for 30 s so a screenshot taken after a stutter still shows it
    struct Hitch
    {
        std::mutex mutex;
        double frameMs = 0.0, noteMs = 0.0;
        std::string note;
        std::chrono::steady_clock::time_point frameAt{}, noteAt{};
    };
    inline Hitch hitch;
    inline std::atomic<uint32_t> longestFlipGapUs{0}; // this window's longest gap between game frames (guestEntry)
    // GOW-Port: GS GL-thread progress, for the EE clock hold (EeScheduler, GOW_VBLANK_CATCHUP): chunks queued and not
    // finished, and when the GL thread last finished one. Work pending with no progress for a while = a host stall.
    inline std::atomic<int64_t> glPendingChunks{0};
    inline std::atomic<uint64_t> glProgressNanos{0};
    inline bool glStalled(uint64_t thresholdNanos)
    {
        if (glPendingChunks.load(std::memory_order_acquire) <= 0)
            return false;
        const uint64_t last = glProgressNanos.load(std::memory_order_acquire);
        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()); // (the same clock as nowNanos below)
        return last != 0u && now > last && now - last > thresholdNanos;
    }
    // GS GL thread: what a slow draw inside the current queued op did (first use of a program, texture decodes...),
    // attached to that op's hitch note and cleared after it
    inline thread_local std::string slowDrawDetail;
    inline void noteHitch(double ms, std::string text)
    {
        std::lock_guard<std::mutex> lock(hitch.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (ms > hitch.noteMs || now - hitch.noteAt > std::chrono::seconds(30))
        {
            hitch.noteMs = ms;
            hitch.note = std::move(text);
            hitch.noteAt = now;
        }
    }
    inline void noteLongFrame(double ms)
    {
        if (ms > 5000.0)
            return; // the app suspended, or a load: not a stutter
        std::lock_guard<std::mutex> lock(hitch.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (ms > hitch.frameMs || now - hitch.frameAt > std::chrono::seconds(30))
        {
            hitch.frameMs = ms;
            hitch.frameAt = now;
        }
    }
    inline std::string hitchLine()
    {
        std::lock_guard<std::mutex> lock(hitch.mutex);
        const auto now = std::chrono::steady_clock::now();
        if (hitch.frameMs <= 0.0 && hitch.note.empty())
            return {};
        char text[448];
        const double frameAgo = std::chrono::duration<double>(now - hitch.frameAt).count();
        const double noteAgo = std::chrono::duration<double>(now - hitch.noteAt).count();
        if (hitch.note.empty() || noteAgo > 30.0)
            std::snprintf(text, sizeof(text), "longest frame %.0f ms (%.0f s ago) | no GL hitch > 25 ms in 30 s", hitch.frameMs, frameAgo);
        else
            std::snprintf(text, sizeof(text), "longest frame %.0f ms (%.0f s ago) | GL hitch %s (%.0f s ago)", hitch.frameMs, frameAgo,
                          hitch.note.c_str(), noteAgo);
        return text;
    }

    // GOW-Port: GOW_GL_AB=1 (iOS default in this test build): the GS renderer's GL thread cycles through candidate
    // settings, one per 5 s report window, and measures its busy time per presented frame under each (gs_threaded_backend
    // report). Every setting draws the same picture. -1 = off; 0 as shipped, 1 GL state cache, 2 uber program only,
    // 3 vertex uploads by BufferSubData instead of mapping.
    constexpr int kAbConfigs = 4;
    inline const char *const kAbNames[kAbConfigs] = {"base", "cache", "uber", "bufsub"};
    inline std::atomic<int> abConfig{-1};
    inline std::mutex abLineMutex;
    inline std::string abLine;
    inline void setAbLine(std::string line)
    {
        std::lock_guard<std::mutex> lock(abLineMutex);
        abLine = std::move(line);
    }
    inline std::string getAbLine()
    {
        std::lock_guard<std::mutex> lock(abLineMutex);
        return abLine;
    }
    inline thread_local Accounting accounting;
    inline uint64_t nowNanos()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    inline void switchTo(Bucket next)
    {
        const auto charge = accounting.switchTo(next, nowNanos());
        if (charge.bucket != Bucket::None)
            nanos[static_cast<size_t>(charge.bucket)].fetch_add(charge.nanos, std::memory_order_relaxed);
    }
    class Scope
    {
        bool on;
        Bucket previous = Bucket::None;
    public:
        explicit Scope(Bucket bucket) : on(enabled())
        {
            if (on) { previous = accounting.active; switchTo(bucket); }
        }
        ~Scope() { if (on) switchTo(previous); }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };
    // GOW-Port: GOW_DELTA_DIAG=1 = game-speed meter (idea from KIexster's ps2recomp-game-speed.patch).
    // On every entry to GOW_PERF_FRAME_PC (vid::Flip, once per game frame) it adds the game's own frame
    // step (float at 0x29C64C in SCUS-97399, seconds) and once per wall second prints
    // [gow-speed] ... speed=<game seconds / wall seconds>: 1.00 = real time, >1 fast, <1 slow motion.
    // Guest code runs one function at a time, so the plain statics need no lock.
    inline bool deltaDiag()
    {
        static const bool on = [] {
            const char *value = std::getenv("GOW_DELTA_DIAG");
            return value && std::strcmp(value, "1") == 0;
        }();
        return on;
    }
    inline void gameSpeedFrame(const uint8_t *rdram)
    {
        static uint64_t windowStart = nowNanos(), frames = 0;
        static double gameSeconds = 0.0, minDelta = 1e9, maxDelta = 0.0;
        float delta = 0.0f;
        std::memcpy(&delta, rdram + 0x29C64Cu, sizeof(delta));
        if (delta > 0.0f && delta < 1.0f)
        {
            gameSeconds += delta;
            if (delta < minDelta) minDelta = delta;
            if (delta > maxDelta) maxDelta = delta;
        }
        // T0 ticks the game measured since its previous frame (0x29C670), in tenths of an NTSC field (9610 ticks).
        static uint32_t hist[24] = {};
        int32_t ticks = 0;
        std::memcpy(&ticks, rdram + 0x29C670u, sizeof(ticks));
        ++hist[std::min<uint32_t>(23u, static_cast<uint32_t>(std::max(0, ticks)) / 961u)];
        ++frames;
        const uint64_t now = nowNanos();
        const double wall = static_cast<double>(now - windowStart) * 1e-9;
        if (wall < 1.0) return;
        std::string line;
        for (uint32_t i = 0; i < 24u; ++i)
            if (hist[i]) line += " " + std::to_string(i) + ":" + std::to_string(hist[i]), hist[i] = 0;
        std::fprintf(stderr, "[gow-speed-hist] field/10 counts:%s\n", line.c_str());
        std::fprintf(stderr, "[gow-speed] frames=%llu wall=%.2f game=%.3f speed=%.3f delta_min=%.4f delta_max=%.4f\n",
                     static_cast<unsigned long long>(frames), wall, gameSeconds, gameSeconds / wall,
                     frames && maxDelta > 0.0 ? minDelta : 0.0, maxDelta);
        latest.speed.store(static_cast<float>(gameSeconds / wall), std::memory_order_relaxed);
        windowStart = now; frames = 0; gameSeconds = 0.0; minDelta = 1e9; maxDelta = 0.0;
    }
    inline void guestEntry(uint32_t pc, const uint8_t *rdram = nullptr)
    {
        static const bool frames = countFrames(), speed = deltaDiag();
        if (!frames && !speed) return;
        static const uint32_t framePc = [] {
            const char *value = std::getenv("GOW_PERF_FRAME_PC");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
        }();
        // Solo entradas a la función elegida: las reanudaciones no cuentan como cuadros nuevos.
        if (framePc == 0u || pc != framePc) return;
        {
            // the gap since the previous game frame (only this thread runs guest code), largest per report window
            static uint64_t lastFlip = 0;
            const uint64_t now = nowNanos();
            if (lastFlip)
            {
                const uint32_t gapUs = static_cast<uint32_t>(std::min<uint64_t>((now - lastFlip) / 1000u, 0xFFFFFFFFu));
                uint32_t seen = longestFlipGapUs.load(std::memory_order_relaxed);
                while (gapUs > seen && !longestFlipGapUs.compare_exchange_weak(seen, gapUs, std::memory_order_relaxed))
                {
                }
            }
            lastFlip = now;
        }
        if (frames) guestFlips.fetch_add(1, std::memory_order_relaxed);
        if (speed && rdram) gameSpeedFrame(rdram);
    }
    inline void report()
    {
        if (!countFrames()) return;
        // Se llama Ãºnicamente desde el hilo de presentaciÃ³n. No lee RAM del juego.
        const uint64_t now = nowNanos();
        static uint64_t first = now, last = now;
        static std::array<uint64_t, bucketCount> before{};
        static uint64_t previousFlips = 0, previousFrames = 0;
        if (now - last < 5000000000ull) return;
        const double seconds = static_cast<double>(now - last) * 1e-9;
        std::array<double, bucketCount> ms{};
        for (size_t i = 0; i < bucketCount; ++i)
        {
            const uint64_t total = nanos[i].load(std::memory_order_relaxed);
            ms[i] = static_cast<double>(total - before[i]) * 1e-6;
            before[i] = total;
        }
        const uint64_t flips = guestFlips.load(std::memory_order_relaxed);
        const uint64_t frames = hostFrames.load(std::memory_order_relaxed);
        std::fprintf(stderr, "[gow-perf] t=%.2f window=%.2f ee_ms=%.2f vu_ms=%.2f gs_ms=%.2f iop_ms=%.2f guest_wait_ms=%.2f upload_ms=%.2f host_wait_ms=%.2f guest_flip_hz=%.2f host_hz=%.2f gsq_wait_ms=%.2f\n",
            (now - first) * 1e-9, seconds, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6],
            (flips - previousFlips) / seconds, (frames - previousFrames) / seconds, ms[7]);
        noteLongFrame(longestFlipGapUs.exchange(0u, std::memory_order_relaxed) * 1e-3);
        latest.guestHz.store(static_cast<float>((flips - previousFlips) / seconds), std::memory_order_relaxed);
        latest.hostHz.store(static_cast<float>((frames - previousFrames) / seconds), std::memory_order_relaxed);
        for (size_t i = 0; i < bucketCount; ++i)
            latest.corePercent[i].store(static_cast<float>(ms[i] / (seconds * 10.0)), std::memory_order_relaxed);
        previousFlips = flips; previousFrames = frames; last = now;
    }
}
#endif
