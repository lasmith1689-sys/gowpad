// GOW-Port: the pause menu (ps2_pause_menu.cpp): Resume, and Save / Load in three save-state slots (ps2_savestate.h).
// Opened by the menu button drawn at the top of the screen, L3+R3 on a controller, or F2 on a keyboard; the emulation
// stops while it is open and the game sees no input.
#pragma once

namespace ps2_pause_menu
{
    // host thread, between BeginDrawing and EndDrawing (after the picture and the on-screen controls)
    void updateAndDraw();
    // any thread: the menu is open (the pad reader then reports nothing pressed)
    bool isOpen();
}
