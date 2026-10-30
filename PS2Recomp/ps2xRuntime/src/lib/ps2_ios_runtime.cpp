// GOW-Port: iOS / iPadOS start-up (main.cpp calls ps2xIosPrepare first).
// - The bundle's game/ folder holds the read-only game files (guest ELF, IRX modules, IOP_MOD/, TOC, a blank memory card).
// - The disc's two large files (PART1/PART2.PAK) come as Apple-hosted asset packs downloaded with the app (iOS 26+,
//   ps2_ios_assets.mm); a disc image in Documents takes precedence over them.
// - Documents (visible in the Files app, and on a PC through Apple Devices / iTunes file sharing) holds what the user
//   adds or keeps: optionally the disc image (any single .iso), the memory card (GodOfWar.ps2, made from the blank one
//   on first start), log.txt (stdout/stderr of the last run; the run before is log-prev.txt) and an optional env.txt
//   with KEY=VALUE lines that override the defaults below.
// - The settings play.cmd sets on Windows are defaults here: GS work on its worker thread, the asynchronous GS FINISH,
//   the system's default sound output. The working directory is Documents (captures and dumps land there).
#if defined(PS2X_IOS)

#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

bool ps2xIosAssetPackFile(const char *packId, const char *file, std::string &path); // ps2_ios_assets.mm

namespace
{
    fs::path documentsDir()
    {
        const char *home = std::getenv("HOME");
        return fs::path(home ? home : ".") / "Documents";
    }

    fs::path bundleGameDir()
    {
        char *base = SDL_GetBasePath();
        const fs::path root = base ? fs::path(base) : fs::path(".");
        SDL_free(base);
        std::error_code ec;
        // (iOS bundles are flat; keep the Resources/ layout working too)
        if (fs::exists(root / "Resources" / "game", ec))
            return root / "Resources" / "game";
        return root / "game";
    }

    void loadEnvFile(const fs::path &file)
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
        }
    }

    std::string lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // The disc from GoWPad's Apple-hosted asset packs (gowdisc1 = PART1.PAK, gowdisc2 = PART2.PAK): a folder in Application
    // Support with the bundle's small disc files and a .disc_layout whose PAK entries are the packs' files (absolute
    // paths), read by the extracted-disc source like disc-usa/ on the PC. Returns its guest ELF, or empty.
    fs::path assetPackDisc(const fs::path &game)
    {
        std::string part1, part2;
        if (!ps2xIosAssetPackFile("gowdisc1", "PART1.PAK", part1) || !ps2xIosAssetPackFile("gowdisc2", "PART2.PAK", part2))
            return {};
        std::error_code ec;
        const char *home = std::getenv("HOME");
        const fs::path disc = fs::path(home ? home : ".") / "Library" / "Application Support" / "gowdisc";
        fs::create_directories(disc / "IOP_MOD", ec);
        for (const auto &entry : fs::recursive_directory_iterator(game, ec))
        {
            if (!entry.is_regular_file(ec) || entry.path().filename() == ".disc_layout")
                continue;
            const fs::path target = disc / fs::relative(entry.path(), game, ec);
            if (!fs::exists(target, ec) || fs::file_size(target, ec) != entry.file_size(ec))
                fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
        }
        std::ifstream in(game / ".disc_layout");
        std::ofstream out(disc / ".disc_layout", std::ios::trunc);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t name = line.rfind(' ');
            const std::string file = name == std::string::npos ? std::string() : line.substr(name + 1);
            if (file == "PART1.PAK" || file == "PART2.PAK")
                line = line.substr(0, name + 1) + (file == "PART1.PAK" ? part1 : part2);
            out << line << '\n';
        }
        if (!out)
            return {};
        std::fprintf(stderr, "[ios] disc from the asset packs: %s, %s\n", part1.c_str(), part2.c_str());
        return disc / "SCUS_973.99";
    }
}

// Returns the guest ELF to boot.
fs::path ps2xIosPrepare()
{
    const fs::path docs = documentsDir();
    std::error_code ec;
    fs::create_directories(docs, ec);

    // logs: keep the previous run's, then stdout and stderr into log.txt through one file descriptor (two separately
    // opened descriptors would each keep their own offset and overwrite each other), stdout line-buffered
    const fs::path log = docs / "log.txt";
    if (fs::exists(log, ec))
        fs::rename(log, docs / "log-prev.txt", ec);
    if (std::freopen(log.c_str(), "w", stdout))
    {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
        dup2(fileno(stdout), fileno(stderr));
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }

    chdir(docs.c_str());
    loadEnvFile(docs / "env.txt");

    setenv("PS2X_GS_WORKER", "1", 0);
    setenv("GOW_GS_FINISH_ASINCRONO", "1", 0);
    // GS GL-thread queue: 32 chunks of 64 KB (default 8). The ship fight sends ~150 chunks a frame, so 8 was a tenth of a
    // frame and every GL hiccup stopped the GS stage (the readout's "wait", 19 % on build 581); on the PC with the GL
    // thread slowed to the phone's busy share the frontend's queue wait fell from ~18 % to under 1 %
    setenv("PS2X_GS_QUEUE_CHUNKS", "32", 0);
    setenv("GOW_AUDIO_DEVICE", "default", 0);
    setenv("GOW_HEARTBEAT", "5", 0); // EE progress in the log every 5 s (ps2_runtime.cpp)
    // performance counters in the log and on screen (ps2_perf.h, gs_host_present.cpp); 0x1837B8 = the game's frame flip
    setenv("GOW_PERF_DIAG", "1", 0);
    setenv("GOW_PERF_FRAME_PC", "0x001837B8", 0);
    setenv("GOW_DELTA_DIAG", "1", 0);
    // (no on-screen readout: the counters and [gs-hitch] causes go to log.txt only; GOW_PERF_HUD=1 in env.txt shows it)
    // one bilinear tap to present (as on the Switch): the bicubic + sharpen pass now runs on every panel pixel (2622x1206)
    // and its GPU time competes with the GS renderer's; the 2x frame is only magnified ~1.35x
    setenv("GOW_SCALER", "bilinear", 0);
    // render targets at 3x the PS2's (448 lines -> 1344): at least the panel's height (1206 on the iPhone 17 Pro Max), so
    // the picture is scaled down to fit instead of up (2x was soft)
    setenv("PS2X_GS_HW_SCALE", "3", 0);
    // GL state cache (gs_gl_cache.cpp): the GS GL thread was the limit, ~2/3 of it in Apple's GL driver; on the PC the
    // cache skips ~5000 redundant calls per frame and was verified call by call (PS2X_GS_STATE_CACHE_VERIFY, 0 mismatches)
    setenv("PS2X_GS_STATE_CACHE", "1", 0);
    // GS draw pipelines pre-warmed at start-up (gs_hw_backend prewarmPipelines): the list from PC runs in the bundle,
    // plus every one this device has drawn (Apple's GL built each at its first draw: ~90 ms stutters)
    setenv("PS2X_GS_PIPELINES_BUNDLED", (bundleGameDir() / "gs_pipelines.txt").c_str(), 0);
    setenv("PS2X_GS_PIPELINES", (docs / "gs_pipelines.txt").c_str(), 0);
    // (GOW_GL_AB=1 in env.txt reruns the on-device comparison; build 511 on the iPhone 17 Pro Max, GL ms per frame:
    // cache 3.06, as before 3.75, uber program only 8.51, BufferSubData uploads 11.28: the shipped settings are the fastest)

    const fs::path game = bundleGameDir();

    // memory card in Documents, started from the bundle's blank card
    const fs::path card = docs / "GodOfWar.ps2";
    if (!fs::exists(card, ec) && fs::exists(game / "blank.ps2", ec))
        fs::copy_file(game / "blank.ps2", card, ec);
    setenv("GOW_MC0", card.c_str(), 0);

    // the disc image: the first .iso in Documents
    if (!std::getenv("GOW_ISO"))
    {
        for (const auto &entry : fs::directory_iterator(docs, ec))
            if (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".iso")
            {
                setenv("GOW_ISO", entry.path().c_str(), 1);
                break;
            }
    }
    std::fprintf(stderr, "[ios] game files: %s, memory card: %s\n", game.c_str(), card.c_str());
    if (const char *iso = std::getenv("GOW_ISO"))
    {
        std::fprintf(stderr, "[ios] disc image: %s\n", iso);
        return game / "SCUS_973.99";
    }
    if (const fs::path elf = assetPackDisc(game); !elf.empty())
        return elf;
    std::fprintf(stderr, "[ios] no disc: neither the asset packs nor a .iso in %s (Files app)\n", docs.c_str());
    return game / "SCUS_973.99";
}

#endif // PS2X_IOS
