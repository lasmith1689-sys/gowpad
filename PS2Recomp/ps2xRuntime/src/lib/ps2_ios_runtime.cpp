// GOW-Port: iOS / iPadOS start-up (main.cpp calls ps2xIosPrepare first).
// - The bundle's game/ folder holds the read-only game files (guest ELF, IRX modules, IOP_MOD/, TOC, a blank memory card).
// - Documents (visible in the Files app, and on a PC through Apple Devices / iTunes file sharing) holds what the user
//   adds or keeps: the disc image (any single .iso), the memory card (GodOfWar.ps2, made from the blank one on first
//   start), log.txt (stdout/stderr of the last run; the run before is log-prev.txt) and an optional env.txt with
//   KEY=VALUE lines that override the defaults below.
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
    setenv("GOW_AUDIO_DEVICE", "default", 0);

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
    if (const char *iso = std::getenv("GOW_ISO"))
        std::fprintf(stderr, "[ios] disc image: %s\n", iso);
    else
        std::fprintf(stderr, "[ios] no .iso in %s: copy the God of War (USA) disc image there with the Files app\n", docs.c_str());
    std::fprintf(stderr, "[ios] game files: %s, memory card: %s\n", game.c_str(), card.c_str());
    return game / "SCUS_973.99";
}

#endif // PS2X_IOS
