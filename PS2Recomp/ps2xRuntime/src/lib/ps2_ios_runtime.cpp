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
