// GOW-Port: contexto host, selección de frames y dibujo raylib usados por el port.
#include "runtime/gs/gs_host_present.h"
#include "rlgl.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_perf.h"
#include "ps2_log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#if defined(PS2X_IOS)
#include <SDL.h>
#include "runtime/ps2_touch_pad.h"
void ps2xIosWaitDisplayRefresh(); // gs_display_ios.mm
#endif

namespace
{
    constexpr uint32_t kWidth = 640u;
    constexpr uint32_t kHeight = 512u;
    constexpr uint32_t kDisplayHeight = 448u;
}


namespace
{
    // GOW-Port: output scaler. The GS renders at its native resolution (512x448); this pass maps that image onto the window
    // at the window's resolution with a 5-tap Catmull-Rom bicubic (GOW_SCALER=sharp, default) instead of the plain
    // bilinear stretch (GOW_SCALER=bilinear), then applies a light contrast-adaptive sharpen. It does not add detail
    // (that needs a scaled GS render target); it removes the blur of the stretch.
    const char *kScalerFragment = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
uniform float sharpen;
uniform float plain;
out vec4 finalColor;

vec4 tex(vec2 uv) { return texture(texture0, uv); }

void main()
{
    if (plain > 0.5)
    {
        // frame at least as large as its window rectangle: one bilinear tap, no bicubic / sharpen
        finalColor = vec4(texture(texture0, fragTexCoord).rgb, 1.0);
        return;
    }
    vec2 pos = fragTexCoord * srcSize;
    vec2 center = floor(pos - 0.5) + 0.5;
    vec2 f = pos - center;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    vec2 w3 = 0.5 * f3 - 0.5 * f2;
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 tc0 = (center - 1.0) / srcSize;
    vec2 tc3 = (center + 2.0) / srcSize;
    vec2 tc12 = (center + offset12) / srcSize;
    vec4 c = tex(vec2(tc12.x, tc0.y)) * (w12.x * w0.y)
           + tex(vec2(tc0.x, tc12.y)) * (w0.x * w12.y)
           + tex(vec2(tc12.x, tc12.y)) * (w12.x * w12.y)
           + tex(vec2(tc3.x, tc12.y)) * (w3.x * w12.y)
           + tex(vec2(tc12.x, tc3.y)) * (w12.x * w3.y);
    float norm = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    c /= norm;
    // contrast-adaptive sharpen from the four direct neighbours of the nearest source texel
    vec2 px = 1.0 / srcSize;
    vec3 n = tex(fragTexCoord + vec2(0.0, -px.y)).rgb, s = tex(fragTexCoord + vec2(0.0, px.y)).rgb;
    vec3 e = tex(fragTexCoord + vec2(px.x, 0.0)).rgb, w = tex(fragTexCoord + vec2(-px.x, 0.0)).rgb;
    vec3 lo = min(min(n, s), min(e, w)), hi = max(max(n, s), max(e, w));
    vec3 amp = clamp(min(lo, 1.0 - hi) / max(hi, vec3(0.0001)), 0.0, 1.0);
    float peak = -1.0 / mix(8.0, 5.0, sharpen);
    vec3 wgt = sqrt(amp) * peak;
    vec3 sharp = (c.rgb + (n + s + e + w) * wgt) / (1.0 + 4.0 * wgt);
    finalColor = vec4(clamp(sharp, 0.0, 1.0), 1.0);
}
)GLSL";

    Shader g_scaler{};
    int g_scalerSrcSize = -1, g_scalerSharpen = -1, g_scalerPlain = -1;
    bool g_scalerTried = false;

#if defined(PS2X_IOS)
    // GOW-Port: the last lines of log.txt (the working directory is Documents), re-read twice a second
    void drawLogOverlay()
    {
        static std::vector<std::string> lines;
        static double readAt = -1.0;
        if (GetTime() - readAt > 0.5)
        {
            readAt = GetTime();
            lines.clear();
            if (std::FILE *f = std::fopen("log.txt", "rb"))
            {
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                const long from = size > 6000 ? size - 6000 : 0;
                std::fseek(f, from, SEEK_SET);
                std::string text(static_cast<size_t>(size - from), '\0');
                text.resize(std::fread(text.data(), 1, text.size(), f));
                std::fclose(f);
                size_t start = from > 0 ? text.find('\n') + 1 : 0;
                while (start < text.size())
                {
                    size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                        end = text.size();
                    std::string line = text.substr(start, end - start);
                    if (!line.empty() && line.rfind("INFO:", 0) != 0) // raylib's start-up chatter
                        lines.push_back(line);
                    start = end + 1;
                }
            }
        }
        const int height = GetScreenHeight(), width = GetScreenWidth();
        const int fontSize = std::max(10, height / 30);
        const int rows = std::max(1, height / (fontSize + 2) - 1);
        DrawRectangle(0, 0, width, height, Color{0, 0, 0, 200});
        DrawText("GoWPad log (Documents/log.txt)", 8, 4, fontSize, YELLOW);
        const size_t first = lines.size() > static_cast<size_t>(rows - 1) ? lines.size() - static_cast<size_t>(rows - 1) : 0u;
        int y = 4 + fontSize + 2;
        for (size_t i = first; i < lines.size(); ++i, y += fontSize + 2)
        {
            std::string line = lines[i];
            while (line.size() > 8 && MeasureText(line.c_str(), fontSize) > width - 16)
                line.resize(line.size() - 4);
            DrawText(line.c_str(), 8, y, fontSize, RAYWHITE);
        }
    }

    // GOW-Port: raylib's SDL platform keeps its screen and viewport in points (874x402 on an iPhone 17 Pro Max) while
    // the high-DPI drawable has the panel's pixels (3x): the viewport is set to the drawable every frame, raylib keeps
    // drawing in points, and the picture covers every pixel. Returns pixels per point.
    float useFullDrawable()
    {
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(static_cast<SDL_Window *>(GetWindowHandle()), &width, &height);
        if (width <= 0 || height <= 0 || GetScreenWidth() <= 0)
            return 1.0f;
        rlViewport(0, 0, width, height);
        return static_cast<float>(width) / static_cast<float>(GetScreenWidth());
    }

    // GOW-Port: GOW_PERF_HUD=1: one line of the performance counters (ps2_perf.h, GOW_PERF_DIAG=1 + GOW_DELTA_DIAG=1)
    // at the top of the screen, so a screenshot shows the frame rate and where the time goes
    void drawPerfHud()
    {
        static const bool on = [] { const char *v = std::getenv("GOW_PERF_HUD"); return v && *v == '1'; }();
        if (!on)
            return;
        const auto &l = ps2_perf::latest;
        const auto pct = [&l](ps2_perf::Bucket b) { return l.corePercent[static_cast<size_t>(b)].load(std::memory_order_relaxed); };
        char text[192];
        std::snprintf(text, sizeof(text), "game %.1f fps  speed %.2f  host %.0f  |  EE %.0f%%  VU %.0f%%  GS %.0f%%  IOP %.0f%%  up %.0f%%  wait %.0f%%",
                      l.guestHz.load(std::memory_order_relaxed), l.speed.load(std::memory_order_relaxed),
                      l.hostHz.load(std::memory_order_relaxed), pct(ps2_perf::Bucket::Ee), pct(ps2_perf::Bucket::Vu),
                      pct(ps2_perf::Bucket::Gs), pct(ps2_perf::Bucket::Iop), pct(ps2_perf::Bucket::Upload),
                      pct(ps2_perf::Bucket::GsQueueWait));
        const int fontSize = std::max(8, GetScreenHeight() / 40);
        DrawRectangle(0, 0, MeasureText(text, fontSize) + 12, fontSize + 6, Color{0, 0, 0, 170});
        DrawText(text, 6, 3, fontSize, Color{255, 230, 120, 255});
        // second line: where the GS GL thread's time goes and how many frames the GPU runs behind it
        const std::string gl = ps2_perf::glLine();
        if (!gl.empty())
        {
            char line[224];
            std::snprintf(line, sizeof(line), "%s | GPU behind %.1f", gl.c_str(), l.gpuFramesBehind.load(std::memory_order_relaxed));
            DrawRectangle(0, fontSize + 6, MeasureText(line, fontSize) + 12, fontSize + 6, Color{0, 0, 0, 170});
            DrawText(line, 6, fontSize + 9, fontSize, Color{160, 220, 255, 255});
        }
        // third line: the GOW_GL_AB test's GL milliseconds per frame for each setting (sample windows in brackets)
        int row = 2;
        const std::string ab = ps2_perf::getAbLine();
        if (!ab.empty())
        {
            const int y = row++ * (fontSize + 6);
            DrawRectangle(0, y, MeasureText(ab.c_str(), fontSize) + 12, fontSize + 6, Color{0, 0, 0, 170});
            DrawText(ab.c_str(), 6, y + 3, fontSize, Color{170, 255, 170, 255});
        }
        // the hitch line: the longest game frame and the worst GL-thread event of the last 30 s (ps2_perf.h)
        const std::string hitch = ps2_perf::hitchLine();
        if (!hitch.empty())
        {
            const int y = row * (fontSize + 6);
            DrawRectangle(0, y, MeasureText(hitch.c_str(), fontSize) + 12, fontSize + 6, Color{0, 0, 0, 170});
            DrawText(hitch.c_str(), 6, y + 3, fontSize, Color{255, 170, 170, 255});
        }
    }
#endif

    bool scalerEnabled()
    {
        static const bool on = [] { const char *v = std::getenv("GOW_SCALER"); return !(v && std::strcmp(v, "bilinear") == 0); }();
        return on;
    }

    Shader *scalerShader()
    {
        if (!g_scalerTried)
        {
            g_scalerTried = true;
            if (scalerEnabled())
            {
#if defined(PS2X_IOS)
                // GOW-Port: raylib's OpenGL ES 3.0 build: GLSL ES 3.00 (its default vertex shader feeds the same inputs)
                std::string source = kScalerFragment;
                source.replace(source.find("#version 330"), std::strlen("#version 330"), "#version 300 es\nprecision highp float;");
                g_scaler = LoadShaderFromMemory(nullptr, source.c_str());
#else
                g_scaler = LoadShaderFromMemory(nullptr, kScalerFragment);
#endif
                if (g_scaler.id != 0u && g_scaler.id != rlGetShaderIdDefault())
                {
                    g_scalerSrcSize = GetShaderLocation(g_scaler, "srcSize");
                    g_scalerSharpen = GetShaderLocation(g_scaler, "sharpen");
                    g_scalerPlain = GetShaderLocation(g_scaler, "plain");
                }
                else
                {
                    std::fprintf(stderr, "[gs-present] scaler shader failed, using bilinear\n");
                    g_scaler = {};
                }
            }
        }
        return g_scaler.id != 0u ? &g_scaler : nullptr;
    }
}

void GSHostPresentation::captureHostContext()
{
    if (started)
        return;
    started = true;
    const char *setting = std::getenv("PS2X_GS_DIRECT_PRESENT");
    // GOW-Port: shared GL textures (no readback/upload) by default; PS2X_GS_DIRECT_PRESENT=0 forces RAM frames.
    captureRequested = !(setting && *setting == '0');
    if (captureRequested)
        GSSharedPresent::CaptureHostContext();
}

void GSHostPresentation::restoreHostContext()
{
    if (captureRequested)
        GSSharedPresent::RestoreHostContext();
}

void GSHostPresentation::createFallback()
{
    if (fallback.id != 0u)
        return;
    Image blank = GenImageColor(kWidth, kHeight, BLANK);
    fallback = LoadTextureFromImage(blank);
    UnloadImage(blank);
    SetTextureFilter(fallback, TEXTURE_FILTER_POINT); // Coincide con la ruta RAM original.
    upload.resize(static_cast<size_t>(kWidth) * kHeight * 4u, 0u);
}

void GSHostPresentation::blankFallback()
{
    if (!uploadedBlank)
    {
        for (size_t offset = 0u; offset < upload.size(); offset += 4u)
        {
            upload[offset] = MAGENTA.r;
            upload[offset + 1u] = MAGENTA.g;
            upload[offset + 2u] = MAGENTA.b;
            upload[offset + 3u] = MAGENTA.a;
        }
        UpdateTexture(fallback, upload.data());
        uploadedBlank = true;
    }
    current.texture = fallback;
    current.width = kWidth;
    current.height = kDisplayHeight;
    current.borrowed = false;
    current.blank = true;
    uploaded = true;
}

#if defined(__SWITCH__)
namespace
{
    // GOW-Port: [gs-host] every 5 s (GOW_PERF_DIAG): where the host thread's prepare() time goes in RAM-frame mode
    struct HostTiming
    {
        uint64_t latchNanos = 0, copyNanos = 0, lockNanos = 0, passes = 0, latches = 0, unchanged = 0, uploads = 0;
        uint64_t uploadNanos = 0, drawNanos = 0; // held under the GL driver lock: texture upload, draw + swap
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        static uint64_t since(std::chrono::steady_clock::time_point start)
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }
        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(5))
                return;
            std::fprintf(stderr, "[gs-host] passes=%llu latches=%llu unchanged=%llu uploads=%llu latch_ms=%.1f copy_ms=%.1f lock_ms=%.1f upload_ms=%.1f draw_swap_ms=%.1f\n",
                         (unsigned long long)passes, (unsigned long long)latches, (unsigned long long)unchanged,
                         (unsigned long long)uploads, latchNanos * 1e-6, copyNanos * 1e-6, lockNanos * 1e-6,
                         uploadNanos * 1e-6, drawNanos * 1e-6);
            *this = HostTiming{};
            last = now;
        }
    };
    HostTiming s_hostTiming;

    std::unique_lock<std::mutex> lockDriver()
    {
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(GSSharedPresent::DriverMutex());
        s_hostTiming.lockNanos += HostTiming::since(start);
        return lock;
    }
}
#endif

GSHostPresentation::View GSHostPresentation::prepare(GS &gs, uint64_t vsyncTick)
{
    createFallback();
#if defined(__SWITCH__)
    ++s_hostTiming.passes;
    s_hostTiming.report();
#endif
    const bool shared = gs.usesSharedPresentation() && GSSharedPresent::Active();
    if (!modeKnown || shared != sharedMode)
    {
        std::fprintf(stderr, "[gs-present] host mode: %s\n", shared ? "shared texture" : "RAM frames");
        std::fflush(stderr);
        modeKnown = true;
        sharedMode = shared;
        hasLatched = uploaded = uploadedBlank = false;
        lastSharedSequence = 0u;
        current = {};
    }
    const bool needsLatch = !hasLatched || vsyncTick != lastTick;
    if (needsLatch)
    {
#if defined(__SWITCH__)
        const auto latchStart = std::chrono::steady_clock::now();
        gs.latchHostPresentationFrame();
        s_hostTiming.latchNanos += HostTiming::since(latchStart);
        ++s_hostTiming.latches;
#else
        gs.latchHostPresentationFrame(); // waits for the GS thread: before taking the driver lock (renderFrame)
#endif
        lastTick = vsyncTick;
        hasLatched = true;
    }
    current.shared = shared;
    if (shared)
    {
#if defined(__SWITCH__)
        driverLock = lockDriver();
#endif
        // GOW-Port: la llamada anterior ya terminó EndDrawing, incluido el batch.
        // Acquire puede conservar el slot previo; nunca descargar píxeles aquí.
        GSSharedPresent::Frame frame;
        if (GSSharedPresent::Acquire(frame))
        {
            current.frame = frame;
            if (frame.sequence != lastSharedSequence)
            {
                gs.recordAcquiredSharedFrame(frame);
                lastSharedSequence = frame.sequence;
            }
            if (!frame.blank && frame.texture != 0u && frame.width != 0u && frame.height != 0u)
            {
                current.texture = Texture2D{frame.texture, static_cast<int>(frame.width), static_cast<int>(frame.height),
                                            1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                current.width = frame.width;
                current.height = frame.height;
                current.borrowed = true;
                current.blank = false;
                return current;
            }
        }
        blankFallback(); // PMODE apagado conserva el placeholder magenta de RAM.
        return current;
    }
    if (!needsLatch && uploaded)
    {
#if defined(__SWITCH__)
        // no new vsync since the last pass: nothing new to show (redrawing the same picture held the GL driver lock
        // ~7 ms per pass, ~45 times a second, while the GS GL thread waited for it)
        ++s_hostTiming.unchanged;
        frameUnchanged = true;
#endif
        return current;
    }

    scratch.clear();
    uint32_t width = 0u, height = 0u, displayFbp = 0u, sourceFbp = 0u;
    bool preferred = false;
#if defined(__SWITCH__)
    const auto copyStart = std::chrono::steady_clock::now();
    const bool copied = gs.copyLatchedHostPresentationFrame(scratch, width, height, &displayFbp, &sourceFbp, &preferred);
    s_hostTiming.copyNanos += HostTiming::since(copyStart);
    if (!copied)
#else
    if (!gs.copyLatchedHostPresentationFrame(scratch, width, height, &displayFbp, &sourceFbp, &preferred))
#endif
    {
#if defined(__SWITCH__)
        driverLock = lockDriver();
#endif
        blankFallback();
        lastWidth = current.width;
        lastHeight = current.height;
        return current;
    }
#if defined(__SWITCH__)
    // GOW-Port: RAM frames are latched at every vsync but the game flips a new one every 2-5 of them. Uploading and
    // redrawing the same picture cost this thread ~2.7 s of every 5 s on core 0, which it shares with the GS GL thread
    // (and the GL driver lock); an unchanged frame is skipped like an unchanged shared frame (renderFrame).
    if (uploaded && !uploadedBlank && width == shownWidth && height == shownHeight && scratch == shownPixels)
    {
        ++s_hostTiming.unchanged;
        frameUnchanged = true;
        return current;
    }
    ++s_hostTiming.uploads;
    shownPixels = scratch;
    shownWidth = width;
    shownHeight = height;
    // the GL driver lock only for the upload and the drawing (the copy and the check above need no GL)
    driverLock = lockDriver();
#endif
    PS2_IF_AGRESSIVE_LOGS({
        if (uploadDebugCount < 128u || displayFbp != lastDisplayFbp || sourceFbp != lastSourceFbp ||
            preferred != lastPreferred || width != lastWidth || height != lastHeight)
            std::cout << "[frame:upload] idx=" << uploadDebugCount << " tick=" << vsyncTick
                      << " displayFbp=" << displayFbp << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(preferred ? 1u : 0u) << std::endl;
        ++uploadDebugCount;
    });
    lastDisplayFbp = displayFbp;
    lastSourceFbp = sourceFbp;
    lastPreferred = preferred;
    lastWidth = width;
    lastHeight = height;
    if (width > kWidth || height > kHeight)
    {
        // GOW-Port: render-target renderer frames (internal resolution) are uploaded as they are, in a texture of their size.
        if (width > fallbackWidth || height > fallbackHeight)
        {
            UnloadTexture(fallback);
            fallbackWidth = std::max(fallbackWidth, width);
            fallbackHeight = std::max(fallbackHeight, height);
            Image bigBlank = GenImageColor(static_cast<int>(fallbackWidth), static_cast<int>(fallbackHeight), BLANK);
            fallback = LoadTextureFromImage(bigBlank);
            UnloadImage(bigBlank);
            SetTextureFilter(fallback, TEXTURE_FILTER_POINT);
        }
#if defined(__SWITCH__)
        const auto uploadStart = std::chrono::steady_clock::now();
#endif
        UpdateTextureRec(fallback, Rectangle{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)}, scratch.data());
#if defined(__SWITCH__)
        s_hostTiming.uploadNanos += HostTiming::since(uploadStart);
#endif
        uploaded = true;
        uploadedBlank = false;
        current.texture = fallback;
        current.width = width;
        current.height = height;
        current.borrowed = current.blank = false;
        current.frame = {};
        return current;
    }
    std::fill(upload.begin(), upload.end(), 0u);
    const uint32_t copyWidth = std::min(width, kWidth);
    const uint32_t copyHeight = std::min(height, kHeight);
    const size_t sourceRow = static_cast<size_t>(width) * 4u;
    const size_t destinationRow = static_cast<size_t>(kWidth) * 4u;
    const size_t copyRow = static_cast<size_t>(copyWidth) * 4u;
    for (uint32_t y = 0u; y < copyHeight; ++y)
    {
        const size_t sourceOffset = static_cast<size_t>(y) * sourceRow;
        const size_t destinationOffset = static_cast<size_t>(y) * destinationRow;
        if (sourceOffset + copyRow > scratch.size() || destinationOffset + copyRow > upload.size())
            break;
        std::memcpy(upload.data() + destinationOffset, scratch.data() + sourceOffset, copyRow);
    }
#if defined(__SWITCH__)
    const auto uploadStart = std::chrono::steady_clock::now();
#endif
    UpdateTexture(fallback, upload.data());
#if defined(__SWITCH__)
    s_hostTiming.uploadNanos += HostTiming::since(uploadStart);
#endif
    uploaded = true;
    uploadedBlank = false;
    current.texture = fallback;
    current.width = width;
    current.height = height;
    current.borrowed = current.blank = false;
    current.frame = {};
    return current;
}

GSHostPresentation::View GSHostPresentation::renderFrame(GS &gs, uint64_t vsyncTick, const std::function<void()> &overlay)
{
    View selected;
#if defined(PS2X_IOS)
    {
        // one window frame per display refresh, latched right after it (gs_display_ios.mm; no raylib frame limiter)
        ps2_perf::Scope perf(ps2_perf::Bucket::HostWait);
        ps2xIosWaitDisplayRefresh();
    }
#endif
#if defined(__SWITCH__)
    // GOW-Port: with shared textures nothing here waits for the GS thread, and Horizon does not time-slice threads of
    // one priority: the GL backend shares core 0 with this thread, and while this thread drew and swapped at 60 Hz
    // holding the GL driver lock the backend ran one queued chunk per host frame (Citron: lock_wait ~4.4 s of 5 s,
    // 2 game fps). So the window is only drawn when a newer frame was published; otherwise input is polled and this
    // thread sleeps (giving the core to the backend), keeping the last image on screen. A published frame the GPU has
    // not finished (Acquire keeps the previous one) is retried every 8 ms, not on every pass.
    const uint64_t latest = GSSharedPresent::LatestSequence();
    const auto now = std::chrono::steady_clock::now();
    const bool retryDue = latest != triedSequence || now - triedAt >= std::chrono::milliseconds(8);
    // (only while sharing is on: when the host's sharing check turned it off before any frame was published, latest
    // and lastSharedSequence both stay 0 and this branch held the magenta placeholder for the whole run)
    if (sharedMode && GSSharedPresent::Active() && current.texture.id != 0u && (latest == lastSharedSequence || !retryDue))
    {
        if (vsyncTick != lastTick)
        {
            gs.latchHostPresentationFrame(); // shared: only queues the Present op
            lastTick = vsyncTick;
        }
        PollInputEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        selected = current;
        selected.presented = false;
        return selected;
    }
    triedSequence = latest;
    triedAt = now;
#endif
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::Upload);
        selected = prepare(gs, vsyncTick);
    }
#if defined(__SWITCH__)
    if (frameUnchanged)
    {
        frameUnchanged = false;
        PollInputEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        selected.presented = false;
        return selected;
    }
#endif
#if defined(__SWITCH__)
    const auto drawStart = std::chrono::steady_clock::now();
#endif
    BeginDrawing();
#if defined(PS2X_IOS)
    const float pixelsPerPoint = useFullDrawable();
#else
    constexpr float pixelsPerPoint = 1.0f;
#endif
    ClearBackground(BLACK);
    const float sourceWidth = static_cast<float>(std::max<uint32_t>(1u, selected.width));
    const float sourceHeight = static_cast<float>(std::max<uint32_t>(1u, selected.height));
    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    // GOW-Port: a PS2 frame (e.g. 512x448) is shown at the TV's aspect, not with square pixels:
    // always 16:9 (the game renders widescreen, see gow_overrides.cpp; 4:3 is not supported any more).
    constexpr float displayAspect = 16.0f / 9.0f;
    const float destinationWidth = std::min(screenWidth, screenHeight * displayAspect);
    const float destinationHeight = destinationWidth / displayAspect;
    const Rectangle source{0.0f, 0.0f, sourceWidth, sourceHeight};
    const Rectangle destination{(screenWidth - destinationWidth) * 0.5f,
                                (screenHeight - destinationHeight) * 0.5f,
                                destinationWidth, destinationHeight};
    // GOW-Port: textura del PBO con filas en orden; no invertir source.height.
    // The bicubic + sharpen pass only helps when the frame is magnified; a frame at least as large as its window
    // rectangle (PS2X_GS_HW_SCALE high enough, or a small window) is drawn with one linear tap (same shader, "plain").
    const bool magnified = sourceWidth < destinationWidth * pixelsPerPoint || sourceHeight < destinationHeight * pixelsPerPoint;
    Shader *scaler = scalerShader();
    if (!scaler)
        SetTextureFilter(selected.texture, TEXTURE_FILTER_BILINEAR);
    if (scaler)
    {
        const float size[2] = {sourceWidth, sourceHeight};
        static const float sharpen = [] { const char *v = std::getenv("GOW_SHARPEN"); return v ? static_cast<float>(std::atof(v)) : 0.5f; }();
        SetShaderValue(*scaler, g_scalerSrcSize, size, SHADER_UNIFORM_VEC2);
        SetShaderValue(*scaler, g_scalerSharpen, &sharpen, SHADER_UNIFORM_FLOAT);
        const float plain = magnified ? 0.0f : 1.0f;
        SetShaderValue(*scaler, g_scalerPlain, &plain, SHADER_UNIFORM_FLOAT);
        SetTextureFilter(selected.texture, TEXTURE_FILTER_BILINEAR);
        BeginShaderMode(*scaler);
    }
    DrawTexturePro(selected.texture, source, destination, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    if (scaler)
        EndShaderMode();
    if (overlay)
        overlay();
#if defined(PS2X_IOS)
    // GOW-Port: iOS has no console: while the game has shown nothing (the magenta placeholder), the end of
    // Documents/log.txt is drawn on screen, so a screenshot shows what happened (GOW_LOG_OVERLAY=1: always)
    static const bool logOverlayAlways = [] { const char *v = std::getenv("GOW_LOG_OVERLAY"); return v && *v == '1'; }();
    if (selected.blank || logOverlayAlways)
        drawLogOverlay();
    else
        drawPerfHud();
    ps2_touch_pad::updateAndDraw(); // on-screen controls while no controller is connected
#endif
    // GOW-Port: GOW_HOST_SHOT=<n>: at host frame n write the window's back buffer (what is about to be shown) to
    // host_shot.png in the working directory, plus the frame / window sizes, to check presentation without a desktop grab.
    static const long hostShotAt = [] { const char *v = std::getenv("GOW_HOST_SHOT"); return v ? std::atol(v) : -1L; }();
    static long hostFrame = 0;
    if (hostShotAt >= 0 && hostFrame++ == hostShotAt)
    {
        rlDrawRenderBatchActive();
        Image screen = LoadImageFromScreen();
        ExportImage(screen, "host_shot.png");
        UnloadImage(screen);
        std::fprintf(stderr, "[gs-present] host_shot frame=%ux%u tex=%u window=%dx%d shared=%d scaler=%d\n", selected.width, selected.height,
                     selected.texture.id, GetScreenWidth(), GetScreenHeight(), selected.shared ? 1 : 0, scaler ? 1 : 0);
    }
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::HostWait);
        EndDrawing();
    }
#if defined(__SWITCH__)
    s_hostTiming.drawNanos += HostTiming::since(drawStart);
    driverLock = {};
    // 60 Hz pacing outside the driver lock (the window has no vertical-sync swap interval, ps2_runtime.cpp)
    {
        ps2_perf::Scope perf(ps2_perf::Bucket::HostWait);
        constexpr auto period = std::chrono::nanoseconds(16666667);
        const auto done = std::chrono::steady_clock::now();
        if (done < nextPresent)
            std::this_thread::sleep_until(nextPresent);
        nextPresent = std::max(done, nextPresent) + period;
    }
#endif
    return selected;
}

void GSHostPresentation::shutdown(GS &gs)
{
    if (!started && fallback.id == 0u)
        return;
    // GOW-Port: nullptr instala un backend nulo; drena y destruye el worker/contexto compartido.
    gs.setRasterBackend(nullptr);
    restoreHostContext();
    if (fallback.id != 0u)
        UnloadTexture(fallback); // Nunca descargar la Texture2D prestada del productor.
    fallback = {};
    if (captureRequested)
        GSSharedPresent::ShutdownHost();
    started = captureRequested = hasLatched = uploaded = uploadedBlank = modeKnown = sharedMode = false;
    lastTick = lastSharedSequence = 0u;
    current = {};
    scratch.clear();
    upload.clear();
}
