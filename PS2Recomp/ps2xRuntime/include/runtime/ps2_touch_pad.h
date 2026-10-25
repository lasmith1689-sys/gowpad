// GOW-Port: iOS on-screen controls (ps2_touch_pad.cpp): a PS2 pad drawn over the picture and driven by touches, shown
// while no game controller is connected. The host thread reads the fingers and draws; the pad reader (game thread)
// takes the result.
#pragma once
#include <cstdint>

namespace ps2_touch_pad
{
    // host thread, between BeginDrawing and EndDrawing: reads the fingers and draws the controls (nothing while a
    // controller is connected)
    void updateAndDraw();
    // any thread: the buttons held (PS2 pad bits set = pressed) and the sticks (rx, ry, lx, ly as in the pad packet,
    // 0x80 = centre); false while the controls are hidden
    bool read(uint16_t &pressed, uint8_t sticks[4]);
}
