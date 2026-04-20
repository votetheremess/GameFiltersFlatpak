#pragma once

#include <cstdint>

namespace vkBasalt
{
    void initInputBlocker(bool enabled);

    void setInputBlocked(bool blocked);
    bool isInputBlocked();

    // Raw-event handlers called from the shared kbDisplay drain in
    // keyboard_input_x11.cpp. Input_blocker owns the XI2 grab and therefore
    // owns the slave raw-event subscription.
    //
    // onRawMotion: accumulates dx/dy into a virtual cursor position that
    // mouse_input.cpp returns to ImGui while input is blocked. We do NOT
    // call XIWarpPointer — KWin on XWayland silently ignores warps on a
    // grabbed master (empirically observed), so the X server cursor stays
    // frozen no matter what. ImGui already renders its own software cursor
    // (io.MouseDrawCursor = true in imgui_overlay.cpp); feeding it the
    // virtual position is what makes the overlay cursor track physical
    // motion. Parameter is really XIRawEvent* — passed as void* so this
    // header doesn't drag X11/Xlib into every including translation unit.
    // onRawButton: updates a button bitmap (master XQueryPointer mask reads
    // 0 when the feeding slaves are grabbed, so mouse_input.cpp pulls
    // button state from here instead).
    void onRawMotion(void* xiRawEvent);
    void onRawButton(int button, bool pressed);

    // Bit (1 << button_number) set iff held. Valid only while isInputBlocked().
    uint32_t currentButtonMask();

    // Virtual cursor position in window-relative coordinates, seeded from
    // XQueryPointer at grab start and updated from XI_RawMotion deltas.
    // mouse_input.cpp reads this while isInputBlocked() is true instead of
    // XQueryPointer, whose master-pointer position doesn't update when the
    // feeding slaves are grabbed.
    void virtualCursorPos(int& x, int& y);
}
