// GOW-Port: the pause menu (ps2_pause_menu.h). Drawn with raylib on the host thread over the last picture, in units of
// 1/100 of the screen height like the on-screen controls (ps2_touch_pad.cpp). Opened by the menu button (top centre,
// below SELECT/START), L3+R3 on a controller or F2; closed by Resume, Circle / B, L3+R3 or F2 again. While it is open the
// emulation is parked (ps2_savestate::setPaused) and the pad reader reports nothing pressed. Saving over a used slot and
// loading ask for a second press; a successful load closes the menu. Keyboard (PC tests): F5 / F9 = save / load slot 1.
#include "runtime/ps2_pause_menu.h"
#include "runtime/ps2_savestate.h"
#include "raylib.h"
#include "rlgl.h"
#if defined(PS2X_IOS)
#include <SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{
    std::atomic<bool> g_open{false};

    // the menu's cursor: 0 = Resume, then (slot, action) cells: 1 + slot * 2 + (0 save / 1 load)
    int g_cursor = 0;
    int g_armed = -1; // a cell pressed once that needs a second press (overwrite / load)
    std::chrono::steady_clock::time_point g_infoAt{};
    ps2_savestate::SlotInfo g_info[ps2_savestate::kSlots];
    bool g_closeAfterLoad = false;
    std::set<long long> g_fingersSeen; // (iOS) fingers already down last frame: a tap is a finger that just landed

    struct Box
    {
        float x, y, w, h;
        bool contains(Vector2 p) const { return p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h; }
    };

    void refreshSlots()
    {
        for (int i = 0; i < ps2_savestate::kSlots; ++i)
            g_info[i] = ps2_savestate::slotInfo(i);
        g_infoAt = std::chrono::steady_clock::now();
    }

    void open()
    {
        if (g_open.load())
            return;
        g_open.store(true);
        g_cursor = 0;
        g_armed = -1;
        g_closeAfterLoad = false;
        ps2_savestate::clearStatus();
        ps2_savestate::setPaused(true);
        refreshSlots();
    }

    void close()
    {
        if (!g_open.load())
            return;
        g_open.store(false);
        g_armed = -1;
        ps2_savestate::setPaused(false);
    }

    bool busy() { return ps2_savestate::status() == ps2_savestate::Status::Working; }

    // a cell chosen (touch, click or Cross / A)
    void activate(int cell)
    {
        if (busy())
            return;
        if (cell == 0)
        {
            close();
            return;
        }
        const int slot = (cell - 1) / 2, load = (cell - 1) % 2;
        const ps2_savestate::SlotInfo &info = g_info[slot];
        if (load && !info.loadable)
            return;
        const bool needsConfirm = load || info.exists;
        if (needsConfirm && g_armed != cell)
        {
            g_armed = cell;
            return;
        }
        g_armed = -1;
        if (load)
        {
            ps2_savestate::requestLoad(slot);
            g_closeAfterLoad = true;
        }
        else
            ps2_savestate::requestSave(slot);
    }

    struct Layout
    {
        Box button;   // the menu button (always drawn)
        Box panel;
        Box resume;
        Box cells[ps2_savestate::kSlots][2];
        float u;
    };

    Layout layout(float width, float height)
    {
        Layout l;
        const float u = height / 100.0f;
        l.u = u;
        l.button = {width * 0.5f - 6.0f * u, 12.5f * u, 12.0f * u, 8.0f * u};
        const float pw = std::min(width - 8.0f * u, 150.0f * u), ph = 88.0f * u;
        l.panel = {(width - pw) * 0.5f, (height - ph) * 0.5f, pw, ph};
        const float px = l.panel.x + 5.0f * u, inner = pw - 10.0f * u;
        l.resume = {px, l.panel.y + 15.0f * u, inner, 11.0f * u};
        const float cw = (inner - 4.0f * u) * 0.5f;
        for (int i = 0; i < ps2_savestate::kSlots; ++i)
            for (int c = 0; c < 2; ++c)
                l.cells[i][c] = {px + c * (cw + 4.0f * u), l.panel.y + 36.0f * u + i * 14.0f * u, cw, 12.0f * u};
        return l;
    }

    void drawBox(const Box &b, Color fill, Color edge, float roundness = 0.25f)
    {
        DrawRectangleRounded({b.x, b.y, b.w, b.h}, roundness, 8, fill);
        DrawRectangleRoundedLinesEx({b.x, b.y, b.w, b.h}, roundness, 8, 2.0f, edge);
    }

    void centredText(const char *text, float cx, float cy, int size, Color color)
    {
        const int w = MeasureText(text, size);
        DrawText(text, static_cast<int>(cx) - w / 2, static_cast<int>(cy) - size / 2, size, color);
    }

    // presses this frame: taps / clicks (points) and controller / keyboard navigation
    struct Input
    {
        std::vector<Vector2> taps;
        bool toggle = false, up = false, down = false, left = false, right = false, accept = false, back = false;
        bool quickSave = false, quickLoad = false;
    };

    Input readInput(float width, float height)
    {
        Input in;
#if defined(PS2X_IOS)
        std::set<long long> now;
        for (int d = 0; d < SDL_GetNumTouchDevices(); ++d)
        {
            const SDL_TouchID device = SDL_GetTouchDevice(d);
            for (int i = 0; i < SDL_GetNumTouchFingers(device); ++i)
                if (const SDL_Finger *finger = SDL_GetTouchFinger(device, i))
                {
                    const long long id = static_cast<long long>(finger->id);
                    now.insert(id);
                    if (!g_fingersSeen.count(id))
                        in.taps.push_back({finger->x * width, finger->y * height});
                }
        }
        g_fingersSeen.swap(now);
#else
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            in.taps.push_back(GetMousePosition());
#endif
        for (int i = 0; i < 4; ++i)
        {
            if (!IsGamepadAvailable(i))
                continue;
            const bool l3 = IsGamepadButtonDown(i, GAMEPAD_BUTTON_LEFT_THUMB), r3 = IsGamepadButtonDown(i, GAMEPAD_BUTTON_RIGHT_THUMB);
            if ((l3 && IsGamepadButtonPressed(i, GAMEPAD_BUTTON_RIGHT_THUMB)) || (r3 && IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_THUMB)))
                in.toggle = true;
            in.up |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_UP);
            in.down |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_DOWN);
            in.left |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
            in.right |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
            in.accept |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
            in.back |= IsGamepadButtonPressed(i, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
        }
        in.toggle |= IsKeyPressed(KEY_F2);
        if (g_open.load())
        {
            in.up |= IsKeyPressed(KEY_UP);
            in.down |= IsKeyPressed(KEY_DOWN);
            in.left |= IsKeyPressed(KEY_LEFT);
            in.right |= IsKeyPressed(KEY_RIGHT);
            in.accept |= IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_X);
            in.back |= IsKeyPressed(KEY_C) || IsKeyPressed(KEY_BACKSPACE);
        }
        in.quickSave = IsKeyPressed(KEY_F5);
        in.quickLoad = IsKeyPressed(KEY_F9);
        return in;
    }

    void moveCursor(const Input &in)
    {
        // rows: 0 = Resume, 1..kSlots = slots; columns 0 save / 1 load
        int row = g_cursor == 0 ? 0 : 1 + (g_cursor - 1) / 2, col = g_cursor == 0 ? 0 : (g_cursor - 1) % 2;
        if (in.up)
            row = std::max(0, row - 1);
        if (in.down)
            row = std::min(ps2_savestate::kSlots, row + 1);
        if (in.left)
            col = 0;
        if (in.right)
            col = 1;
        const int next = row == 0 ? 0 : 1 + (row - 1) * 2 + col;
        if (next != g_cursor)
            g_armed = -1;
        g_cursor = next;
    }
}

namespace ps2_pause_menu
{
    bool isOpen()
    {
        return g_open.load(std::memory_order_relaxed);
    }

    void updateAndDraw()
    {
        const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
        const Layout l = layout(width, height);
        const float u = l.u;
        const Input in = readInput(width, height);

        // GOW_STATE_TEST=s1@60,l1@90 (tests): at 60 s after start save to slot 1, at 90 s load slot 1 (seconds of host time);
        // p0@t opens the menu (the game stops), c0@t closes it
        static std::vector<std::pair<double, int>> script = []
        {
            std::vector<std::pair<double, int>> steps; // (time, +slot save / -slot load)
            if (const char *v = std::getenv("GOW_STATE_TEST"))
                for (const char *p = v; *p;)
                {
                    const char kind = *p;
                    const int slot = std::atoi(p + 1);
                    const char *at = std::strchr(p, '@');
                    if (!at || !std::strchr("slpc", kind) || ((kind == 's' || kind == 'l') && (slot < 1 || slot > ps2_savestate::kSlots)))
                        break;
                    steps.emplace_back(std::atof(at + 1), kind == 's' ? slot : kind == 'l' ? -slot : kind == 'p' ? 100 : 101);
                    const char *next = std::strchr(at, ',');
                    p = next ? next + 1 : at + std::strlen(at);
                }
            return steps;
        }();
        static const auto scriptStart = std::chrono::steady_clock::now();
        // (screenshots for checking: state_<n>_before.png when a step starts, state_<n>_after.png 30 frames after it)
        static int shotStep = 0, shotCountdown = -1;
        const auto shot = [](const char *when, int n) {
            const char *dir = std::getenv("GOW_STATE_DIR");
            char name[512];
            std::snprintf(name, sizeof(name), "%s/state_%d_%s.png", dir && *dir ? dir : ".", n, when);
            rlDrawRenderBatchActive();
            Image screen = LoadImageFromScreen();
            ExportImage(screen, name);
            UnloadImage(screen);
        };
        if (shotCountdown > 0 && --shotCountdown == 0)
            shot("after", shotStep);
        if (!script.empty() && !busy() &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - scriptStart).count() >= script.front().first)
        {
            const int step = script.front().second;
            script.erase(script.begin());
            if (step >= 100)
            {
                std::fprintf(stderr, "[savestate] test: menu %s\n", step == 100 ? "open" : "closed");
                step == 100 ? open() : close();
                return;
            }
            std::fprintf(stderr, "[savestate] test: %s slot %d\n", step > 0 ? "save" : "load", std::abs(step));
            shot("before", ++shotStep);
            shotCountdown = 30;
            if (step > 0)
                ps2_savestate::requestSave(step - 1);
            else
                ps2_savestate::requestLoad(-step - 1);
        }
        if (!g_open.load())
        {
            if (in.quickSave && !busy())
                ps2_savestate::requestSave(0);
            if (in.quickLoad && !busy() && ps2_savestate::slotInfo(0).loadable)
                ps2_savestate::requestLoad(0);
            bool tapped = in.toggle;
            for (const Vector2 &p : in.taps)
                tapped |= l.button.contains(p);
            // the menu button: two bars, faint so it does not cover the picture
            drawBox(l.button, Color{0, 0, 0, 70}, Color{255, 255, 255, 90}, 0.5f);
            const float bw = 1.6f * u, bh = 4.0f * u, cx = l.button.x + l.button.w * 0.5f, cy = l.button.y + l.button.h * 0.5f;
            DrawRectangleRec({cx - 2.2f * u, cy - bh * 0.5f, bw, bh}, Color{255, 255, 255, 150});
            DrawRectangleRec({cx + 0.6f * u, cy - bh * 0.5f, bw, bh}, Color{255, 255, 255, 150});
            std::string message;
            const ps2_savestate::Status st = ps2_savestate::status(&message);
            if (st != ps2_savestate::Status::Idle && !message.empty()) // (quick save / load from the keyboard)
                centredText(message.c_str(), width * 0.5f, 26.0f * u, static_cast<int>(4.0f * u), Color{255, 255, 255, 200});
            if (tapped)
                open();
            return;
        }

        // open: input
        std::string message;
        const ps2_savestate::Status st = ps2_savestate::status(&message);
        if (g_closeAfterLoad && st == ps2_savestate::Status::Done)
        {
            g_closeAfterLoad = false;
            close();
            return;
        }
        if (st == ps2_savestate::Status::Failed)
            g_closeAfterLoad = false;
        if (std::chrono::steady_clock::now() - g_infoAt > std::chrono::milliseconds(500))
            refreshSlots();
        if ((in.toggle || in.back) && !busy())
        {
            close();
            return;
        }
        moveCursor(in);
        if (in.accept)
            activate(g_cursor);
        for (const Vector2 &p : in.taps)
        {
            if (l.resume.contains(p))
                g_cursor = 0, activate(0);
            for (int i = 0; i < ps2_savestate::kSlots; ++i)
                for (int c = 0; c < 2; ++c)
                    if (l.cells[i][c].contains(p))
                    {
                        const int cell = 1 + i * 2 + c;
                        if (g_cursor != cell)
                            g_armed = -1;
                        g_cursor = cell;
                        activate(cell);
                    }
            if (!g_open.load())
                return;
        }

        // draw
        DrawRectangle(0, 0, static_cast<int>(width), static_cast<int>(height), Color{0, 0, 0, 150});
        drawBox(l.panel, Color{20, 16, 14, 235}, Color{190, 150, 90, 255}, 0.08f);
        const int title = static_cast<int>(7.0f * u), text = static_cast<int>(4.2f * u), small = static_cast<int>(3.3f * u);
        centredText("PAUSED", l.panel.x + l.panel.w * 0.5f, l.panel.y + 8.0f * u, title, Color{235, 205, 150, 255});
        const auto cellColours = [&](int cell, bool enabled, Color &fill, Color &edge)
        {
            const bool focused = g_cursor == cell;
            fill = !enabled ? Color{40, 36, 34, 220} : g_armed == cell ? Color{150, 40, 30, 240} : focused ? Color{110, 80, 45, 240} : Color{55, 45, 38, 230};
            edge = focused ? Color{255, 220, 160, 255} : Color{120, 100, 80, 255};
        };
        Color fill, edge;
        cellColours(0, true, fill, edge);
        drawBox(l.resume, fill, edge);
        centredText("RESUME", l.resume.x + l.resume.w * 0.5f, l.resume.y + l.resume.h * 0.5f, text, WHITE);
        centredText("SAVE", l.cells[0][0].x + l.cells[0][0].w * 0.5f, l.cells[0][0].y - 3.5f * u, small, Color{235, 205, 150, 255});
        centredText("LOAD", l.cells[0][1].x + l.cells[0][1].w * 0.5f, l.cells[0][1].y - 3.5f * u, small, Color{235, 205, 150, 255});
        for (int i = 0; i < ps2_savestate::kSlots; ++i)
            for (int c = 0; c < 2; ++c)
            {
                const int cell = 1 + i * 2 + c;
                const ps2_savestate::SlotInfo &info = g_info[i];
                const bool enabled = c == 0 || info.loadable;
                cellColours(cell, enabled, fill, edge);
                const Box &b = l.cells[i][c];
                drawBox(b, fill, edge);
                char head[48];
                std::snprintf(head, sizeof(head), "SLOT %d", i + 1);
                const char *sub = g_armed == cell ? (c == 0 ? "press again to overwrite" : "press again to load")
                                                  : info.exists ? info.label.c_str() : "empty";
                centredText(head, b.x + b.w * 0.5f, b.y + b.h * 0.32f, text, enabled ? WHITE : Color{130, 130, 130, 255});
                centredText(sub, b.x + b.w * 0.5f, b.y + b.h * 0.72f, small, enabled ? Color{220, 210, 195, 255} : Color{110, 110, 110, 255});
            }
        const char *line = st == ps2_savestate::Status::Working ? (message.empty() ? "Working..." : message.c_str())
                           : !message.empty()                    ? message.c_str()
                                                                 : "";
        centredText(line, l.panel.x + l.panel.w * 0.5f, l.panel.y + l.panel.h - 6.0f * u, small,
                    st == ps2_savestate::Status::Failed ? Color{255, 120, 100, 255} : Color{220, 210, 195, 255});
    }
}
