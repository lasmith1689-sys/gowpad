// GOW-Port: iOS on-screen controls (ps2_touch_pad.h). Drawn in raylib's screen points over the picture while no game
// controller is connected; hidden (and released) the moment one is. Fingers come from SDL's own finger list (raylib's
// touch state keeps a stale count and puts the mouse position in slot 0), read on the host thread only.
//
// Layout, in units of 1/100 of the screen height: a floating left stick (a finger that lands in the left half, not on a
// button, holds it, centred where it landed), the face buttons and a small right stick (God of War's evade) on the
// right, L1/L2 and R1/R2 in the top corners, SELECT/START at the top and a D-pad left of centre. A finger keeps the role
// it got when it landed; a button finger presses whatever it is over, so sliding across buttons works.
#include "runtime/ps2_touch_pad.h"
#include "raylib.h"
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

namespace
{
    constexpr uint16_t PAD_LEFT = 0x0080u, PAD_DOWN = 0x0040u, PAD_RIGHT = 0x0020u, PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u, PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u, PAD_CROSS = 0x4000u, PAD_CIRCLE = 0x2000u, PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u, PAD_L1 = 0x0400u, PAD_R2 = 0x0200u, PAD_L2 = 0x0100u;

    // bit 63 visible, bits 0-15 the buttons held, 16-47 the sticks (rx, ry, lx, ly)
    std::atomic<uint64_t> g_state{0};

    enum class Shape { Triangle, Circle, Cross, Square, Pill, Arrow };
    struct Control
    {
        uint16_t mask;
        Shape shape;
        Vector2 at;
        float w, h;      // circles: w = radius; pills and arrows: the box
        float angle;     // arrows: 0 up, 90 right, 180 down, 270 left
        const char *label;
        Color color;
    };

    enum class Role { Buttons, LeftStick, RightStick };
    struct Finger
    {
        Role role;
        Vector2 origin;
    };
    std::map<SDL_FingerID, Finger> g_fingers;

    struct Layout
    {
        std::vector<Control> controls;
        Vector2 leftStick, rightStick;
        float leftRadius, rightRadius;
    };

    Layout layout(float width, float height)
    {
        const float u = height / 100.0f;
        Layout l;
        // (clear of the Dynamic Island, which reaches ~48 points in from either side at mid height in landscape)
        const Vector2 face{width - 33.0f * u, height - 34.0f * u};
        const float d = 13.0f * u, r = 7.5f * u;
        l.controls.push_back({PAD_TRIANGLE, Shape::Triangle, {face.x, face.y - d}, r, r, 0, nullptr, {64, 226, 160, 255}});
        l.controls.push_back({PAD_CROSS, Shape::Cross, {face.x, face.y + d}, r, r, 0, nullptr, {124, 178, 232, 255}});
        l.controls.push_back({PAD_SQUARE, Shape::Square, {face.x - d, face.y}, r, r, 0, nullptr, {255, 105, 248, 255}});
        l.controls.push_back({PAD_CIRCLE, Shape::Circle, {face.x + d, face.y}, r, r, 0, nullptr, {255, 102, 102, 255}});
        const float sw = 18.0f * u, sh = 9.0f * u;
        l.controls.push_back({PAD_L1, Shape::Pill, {16.0f * u, 16.0f * u}, sw, sh, 0, "L1", WHITE});
        l.controls.push_back({PAD_L2, Shape::Pill, {16.0f * u, 28.0f * u}, sw, sh, 0, "L2", WHITE});
        l.controls.push_back({PAD_R1, Shape::Pill, {width - 16.0f * u, 16.0f * u}, sw, sh, 0, "R1", WHITE});
        l.controls.push_back({PAD_R2, Shape::Pill, {width - 16.0f * u, 28.0f * u}, sw, sh, 0, "R2", WHITE});
        l.controls.push_back({PAD_SELECT, Shape::Pill, {width * 0.5f - 10.0f * u, 7.0f * u}, 16.0f * u, 6.5f * u, 0, "SELECT", WHITE});
        l.controls.push_back({PAD_START, Shape::Pill, {width * 0.5f + 10.0f * u, 7.0f * u}, 16.0f * u, 6.5f * u, 0, "START", WHITE});
        const Vector2 dpad{54.0f * u, height - 15.0f * u};
        const float a = 7.0f * u, b = 7.5f * u;
        l.controls.push_back({PAD_UP, Shape::Arrow, {dpad.x, dpad.y - a}, b, b, 0, nullptr, WHITE});
        l.controls.push_back({PAD_RIGHT, Shape::Arrow, {dpad.x + a, dpad.y}, b, b, 90, nullptr, WHITE});
        l.controls.push_back({PAD_DOWN, Shape::Arrow, {dpad.x, dpad.y + a}, b, b, 180, nullptr, WHITE});
        l.controls.push_back({PAD_LEFT, Shape::Arrow, {dpad.x - a, dpad.y}, b, b, 270, nullptr, WHITE});
        l.leftStick = {28.0f * u, height - 30.0f * u};
        l.leftRadius = 13.0f * u;
        l.rightStick = {width - 60.0f * u, height - 14.0f * u};
        l.rightRadius = 8.5f * u;
        return l;
    }

    bool hits(const Control &c, Vector2 p)
    {
        if (c.shape == Shape::Pill || c.shape == Shape::Arrow)
        {
            const float mx = c.w * 0.5f + 4.0f, my = c.h * 0.5f + 4.0f; // a little slack around the box
            return std::fabs(p.x - c.at.x) <= mx && std::fabs(p.y - c.at.y) <= my;
        }
        const float dx = p.x - c.at.x, dy = p.y - c.at.y, reach = c.w * 1.25f; // neighbours overlap: two at once
        return dx * dx + dy * dy <= reach * reach;
    }

    Vector2 stickVector(Vector2 p, Vector2 origin, float radius)
    {
        Vector2 v{(p.x - origin.x) / radius, (p.y - origin.y) / radius};
        const float length = std::sqrt(v.x * v.x + v.y * v.y);
        if (length < 0.12f)
            return {0.0f, 0.0f};
        if (length > 1.0f)
            v.x /= length, v.y /= length;
        return v;
    }

    uint8_t axis(float v)
    {
        return static_cast<uint8_t>(std::clamp(128.0f + v * 127.0f, 0.0f, 255.0f));
    }

    Vector2 rotate(Vector2 v, float degrees)
    {
        const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        return {v.x * c - v.y * s, v.x * s + v.y * c};
    }

    void draw(const Control &c, bool down)
    {
        const unsigned char fill = down ? 150 : 55, edge = down ? 230 : 140;
        const Color tint{c.color.r, c.color.g, c.color.b, edge};
        switch (c.shape)
        {
        case Shape::Pill:
        {
            const Rectangle box{c.at.x - c.w * 0.5f, c.at.y - c.h * 0.5f, c.w, c.h};
            DrawRectangleRounded(box, 0.6f, 8, Color{255, 255, 255, fill});
            const int size = std::max(8, static_cast<int>(c.h * 0.5f));
            const int textWidth = MeasureText(c.label, size);
            DrawText(c.label, static_cast<int>(c.at.x) - textWidth / 2, static_cast<int>(c.at.y) - size / 2, size, Color{255, 255, 255, edge});
            break;
        }
        case Shape::Arrow:
        {
            const Rectangle box{c.at.x - c.w * 0.5f, c.at.y - c.h * 0.5f, c.w, c.h};
            DrawRectangleRounded(box, 0.3f, 6, Color{255, 255, 255, fill});
            const float s = c.w * 0.28f;
            // counter-clockwise on screen (raylib fills that winding); rotation keeps it
            const Vector2 p0 = rotate({0.0f, -s}, c.angle), p1 = rotate({-s, s * 0.7f}, c.angle), p2 = rotate({s, s * 0.7f}, c.angle);
            DrawTriangle({c.at.x + p0.x, c.at.y + p0.y}, {c.at.x + p1.x, c.at.y + p1.y}, {c.at.x + p2.x, c.at.y + p2.y}, Color{255, 255, 255, edge});
            break;
        }
        default:
        {
            const float r = c.w, s = r * 0.42f, t = std::max(2.0f, r * 0.12f);
            DrawCircleV(c.at, r, Color{255, 255, 255, fill});
            if (c.shape == Shape::Triangle)
            {
                const Vector2 a{c.at.x, c.at.y - s * 1.05f}, b{c.at.x - s, c.at.y + s * 0.75f}, d{c.at.x + s, c.at.y + s * 0.75f};
                DrawLineEx(a, b, t, tint), DrawLineEx(b, d, t, tint), DrawLineEx(d, a, t, tint);
            }
            else if (c.shape == Shape::Circle)
                DrawRing(c.at, s - t * 0.5f, s + t * 0.5f, 0.0f, 360.0f, 32, tint);
            else if (c.shape == Shape::Cross)
            {
                DrawLineEx({c.at.x - s, c.at.y - s}, {c.at.x + s, c.at.y + s}, t, tint);
                DrawLineEx({c.at.x - s, c.at.y + s}, {c.at.x + s, c.at.y - s}, t, tint);
            }
            else
                DrawRectangleLinesEx({c.at.x - s * 0.85f, c.at.y - s * 0.85f, s * 1.7f, s * 1.7f}, t, tint);
            break;
        }
        }
    }

    void drawStick(Vector2 origin, float radius, Vector2 v, bool held)
    {
        DrawCircleV(origin, radius, Color{255, 255, 255, static_cast<unsigned char>(held ? 60 : 35)});
        DrawRing(origin, radius - 1.5f, radius + 1.5f, 0.0f, 360.0f, 48, Color{255, 255, 255, 120});
        DrawCircleV({origin.x + v.x * radius, origin.y + v.y * radius}, radius * 0.45f,
                    Color{255, 255, 255, static_cast<unsigned char>(held ? 170 : 90)});
    }
}

namespace ps2_touch_pad
{
    void updateAndDraw()
    {
        // GOW_TOUCH_PAD=1 shows the controls even with a controller connected, =0 never
        static const char *setting = std::getenv("GOW_TOUCH_PAD");
        bool controller = false;
        for (int i = 0; i < 4 && !controller; ++i)
            controller = IsGamepadAvailable(i);
        if (setting && *setting == '1')
            controller = false;
        else if (setting && *setting == '0')
            controller = true;
        if (controller)
        {
            g_state.store(0u, std::memory_order_release);
            g_fingers.clear();
            return;
        }
        const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
        const Layout l = layout(width, height);

        std::map<SDL_FingerID, Vector2> now;
        for (int d = 0; d < SDL_GetNumTouchDevices(); ++d)
        {
            const SDL_TouchID device = SDL_GetTouchDevice(d);
            for (int i = 0; i < SDL_GetNumTouchFingers(device); ++i)
                if (const SDL_Finger *finger = SDL_GetTouchFinger(device, i))
                    now[finger->id] = {finger->x * width, finger->y * height};
        }
        for (auto it = g_fingers.begin(); it != g_fingers.end();)
            it = now.count(it->first) ? std::next(it) : g_fingers.erase(it);
        bool leftHeld = false, rightHeld = false;
        for (const auto &[id, finger] : g_fingers)
            leftHeld |= finger.role == Role::LeftStick, rightHeld |= finger.role == Role::RightStick;
        for (const auto &[id, p] : now)
        {
            if (g_fingers.count(id))
                continue;
            Finger finger{Role::Buttons, p};
            const bool onButton = std::any_of(l.controls.begin(), l.controls.end(), [&](const Control &c) { return hits(c, p); });
            const float rx = p.x - l.rightStick.x, ry = p.y - l.rightStick.y, reach = l.rightRadius * 1.6f;
            if (!onButton && !rightHeld && rx * rx + ry * ry <= reach * reach)
                finger = {Role::RightStick, l.rightStick}, rightHeld = true;
            else if (!onButton && !leftHeld && p.x < width * 0.5f && p.y > height * 0.3f)
                finger = {Role::LeftStick, p}, leftHeld = true;
            g_fingers[id] = finger;
        }

        uint16_t pressed = 0;
        Vector2 left{0.0f, 0.0f}, right{0.0f, 0.0f};
        Vector2 leftOrigin = l.leftStick;
        for (const auto &[id, finger] : g_fingers)
        {
            const Vector2 p = now[id];
            if (finger.role == Role::LeftStick)
                left = stickVector(p, finger.origin, l.leftRadius), leftOrigin = finger.origin;
            else if (finger.role == Role::RightStick)
                right = stickVector(p, finger.origin, l.rightRadius);
            else
                for (const Control &c : l.controls)
                    if (hits(c, p))
                        pressed |= c.mask;
        }
        const uint64_t sticks = static_cast<uint64_t>(axis(right.x)) | static_cast<uint64_t>(axis(right.y)) << 8 |
                                static_cast<uint64_t>(axis(left.x)) << 16 | static_cast<uint64_t>(axis(left.y)) << 24;
        g_state.store(1ull << 63 | sticks << 16 | pressed, std::memory_order_release);

        for (const Control &c : l.controls)
            draw(c, (pressed & c.mask) != 0);
        drawStick(leftOrigin, l.leftRadius, left, leftHeld);
        drawStick(l.rightStick, l.rightRadius, right, rightHeld);
    }

    bool read(uint16_t &pressed, uint8_t sticks[4])
    {
        const uint64_t state = g_state.load(std::memory_order_acquire);
        if (!(state >> 63))
            return false;
        pressed = static_cast<uint16_t>(state);
        for (int i = 0; i < 4; ++i)
            sticks[i] = static_cast<uint8_t>(state >> (16 + 8 * i));
        return true;
    }
}
