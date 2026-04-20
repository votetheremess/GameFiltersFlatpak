#include "input_blocker.hpp"
#include "keyboard_input_x11.hpp"
#include "logger.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace vkBasalt
{
    static bool blockingEnabled = false;
    static bool blocked = false;
    static bool grabbed = false;

    static std::vector<int> grabbedDevices;

    // Bit (1 << button_number) set iff that mouse button is currently held.
    // Updated exclusively by onRawButton; read by mouse_input.cpp via
    // currentButtonMask() when isInputBlocked() is true.
    static uint32_t g_buttonMask = 0;

    // Virtual cursor position, window-relative. Seeded from XQueryPointer at
    // grab start; onRawMotion accumulates dx/dy into it. Clamped to cached
    // screen bounds so it can't run off. Read via virtualCursorPos() by
    // mouse_input.cpp, whose XQueryPointer path stalls under slave grabs.
    static int g_virtualX = 0;
    static int g_virtualY = 0;
    static int g_screenW = 0;
    static int g_screenH = 0;

    static bool xiQueried  = false;
    static bool xiAvailable = false;

    static bool ensureXI2(Display* display)
    {
        if (xiQueried)
            return xiAvailable;
        xiQueried = true;

        int xiOpcode = 0, event = 0, error = 0;
        if (!XQueryExtension(display, "XInputExtension", &xiOpcode, &event, &error))
        {
            Logger::info("input_blocker: XInputExtension not available; falling back to core grabs");
            return false;
        }
        int major = 2, minor = 0;
        if (XIQueryVersion(display, &major, &minor) == BadRequest)
        {
            Logger::info("input_blocker: XI2 unsupported; falling back to core grabs");
            return false;
        }
        xiAvailable = true;
        return true;
    }

    // Core-X11 legacy grab. Only reached on exotic bare-X11 setups without
    // XInput2; does NOT block XI2 raw events, so Wine/DXVK raw-motion
    // subscriptions leak through. Kept as graceful-degrade fallback.
    static bool coreGrab(Display* display)
    {
        Window root = DefaultRootWindow(display);
        int kb  = XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
        int ptr = XGrabPointer(display, root, False,
                               ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                               GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
        if (kb == GrabSuccess && ptr == GrabSuccess)
            return true;
        if (kb  == GrabSuccess) XUngrabKeyboard(display, CurrentTime);
        if (ptr == GrabSuccess) XUngrabPointer(display, CurrentTime);
        return false;
    }

    // XI2 grab — masters AND non-XTEST slaves.
    //
    // Why both:
    //   * Master grab: redirects normal XI_Motion/Button/Key events so the
    //     game's window-level selections see nothing. Best-effort — KWin on
    //     XWayland sometimes holds a compositor-level master grab that makes
    //     ours fail with AlreadyGrabbed. That's fine; slave grabs are what
    //     actually starve Wine, master grab is defense in depth.
    //   * Slave grab: redirects XI_Raw* events so Wine/DXVK's slave-level
    //     raw-event subscriptions (the path that gives relative-mouse FPS
    //     deltas) see nothing either. This is the load-bearing grab.
    //
    // Why the XTEST filter:
    //   * We don't currently inject via XTest, but grabbing a virtual device
    //     is at best pointless (it has no physical source) and at worst
    //     risky — XTEST slaves are involved in some synthetic-event paths.
    //     Cheap insurance; exclude by name.
    //
    // Cursor problem and how we solve it:
    //   * Grabbing a slave with owner_events=False cuts the slave → master
    //     feed entirely, so the X server cursor freezes mid-screen.
    //   * An earlier attempt drove the master via XIWarpPointer from drained
    //     XI_RawMotion. That approach is correct per XI2 spec but in practice
    //     KWin on XWayland silently ignores warps on a grabbed master; the
    //     cursor stayed frozen no matter what. Removed.
    //   * Current approach: we accumulate dx/dy into g_virtualX/g_virtualY
    //     and hand that back to ImGui via getMouseState(). ImGui is already
    //     rendering its own software cursor (io.MouseDrawCursor = true in
    //     imgui_overlay.cpp), so the overlay cursor tracks physical motion
    //     perfectly even though the X server cursor is frozen. The game
    //     never sees a cursor either way — it's starved of events.
    //
    // Button/keyboard state:
    //   * Master button mask via XQueryPointer reads 0 when the feeding
    //     slave is grabbed. We track button state ourselves in g_buttonMask.
    //   * Keyboard raw events are consumed by the existing drain in
    //     keyboard_input_x11.cpp — works unchanged.
    static void xiGrab(Display* display)
    {
        int ndev = 0;
        XIDeviceInfo* info = XIQueryDevice(display, XIAllDevices, &ndev);
        if (!info)
        {
            Logger::info("input_blocker: XIQueryDevice returned null");
            return;
        }

        unsigned char masterMask[XIMaskLen(XI_LASTEVENT)];
        std::memset(masterMask, 0, sizeof(masterMask));
        XISetMask(masterMask, XI_Motion);
        XISetMask(masterMask, XI_ButtonPress);
        XISetMask(masterMask, XI_ButtonRelease);
        XISetMask(masterMask, XI_KeyPress);
        XISetMask(masterMask, XI_KeyRelease);

        unsigned char slaveMask[XIMaskLen(XI_LASTEVENT)];
        std::memset(slaveMask, 0, sizeof(slaveMask));
        XISetMask(slaveMask, XI_RawMotion);
        XISetMask(slaveMask, XI_RawButtonPress);
        XISetMask(slaveMask, XI_RawButtonRelease);
        XISetMask(slaveMask, XI_RawKeyPress);
        XISetMask(slaveMask, XI_RawKeyRelease);

        Window root = DefaultRootWindow(display);

        int attempted = 0;
        int masters = 0, slaves = 0;
        for (int i = 0; i < ndev; ++i)
        {
            const XIDeviceInfo& d = info[i];

            const bool isMaster =
                d.use == XIMasterPointer || d.use == XIMasterKeyboard;
            const bool isSlave =
                d.use == XISlavePointer  || d.use == XISlaveKeyboard;

            if (!isMaster && !isSlave)
                continue;

            // Skip floating slaves (no routing to any master) and XTEST
            // virtual devices (we don't inject through them; grabbing risks
            // blocking other compositor-level synthesis paths).
            if (isSlave && d.attachment == 0)
                continue;
            if (d.name && std::strstr(d.name, "XTEST") != nullptr)
                continue;

            const unsigned char* mask = isMaster ? masterMask : slaveMask;

            XIEventMask em;
            em.deviceid = d.deviceid;
            em.mask_len = sizeof(masterMask);  // both mask arrays are the same length
            em.mask     = const_cast<unsigned char*>(mask);

            attempted++;
            int rc = XIGrabDevice(display, d.deviceid, root, CurrentTime, None,
                                  GrabModeAsync, GrabModeAsync, False, &em);
            if (rc == GrabSuccess)
            {
                grabbedDevices.push_back(d.deviceid);
                if (isMaster)
                    masters++;
                else
                    slaves++;
            }
            else
            {
                Logger::info("input_blocker: XIGrabDevice failed on device " +
                             std::to_string(d.deviceid) + " (" +
                             (d.name ? d.name : "?") + ", use=" +
                             std::to_string(d.use) + ") rc=" + std::to_string(rc));
            }
        }
        XIFreeDeviceInfo(info);

        Logger::info("input_blocker: XI2 grabbed " +
                     std::to_string(grabbedDevices.size()) + "/" +
                     std::to_string(attempted) + " device(s) [" +
                     std::to_string(masters) + " master, " +
                     std::to_string(slaves) + " slave]");
    }

    static void grabInput()
    {
        if (grabbed)
            return;

        Display* display = (Display*)getKeyboardDisplay();
        if (!display)
        {
            Logger::info("input_blocker: no keyboard display; cannot grab");
            return;
        }

        if (ensureXI2(display))
            xiGrab(display);

        if (grabbedDevices.empty() && !coreGrab(display))
        {
            Logger::info("input_blocker: grab failed on both XI2 and core paths");
            XFlush(display);
            return;
        }

        // Seed virtual cursor from the current master-pointer position so
        // ImGui's software cursor picks up exactly where the X cursor was
        // when the overlay opened. Cache screen bounds so onRawMotion can
        // clamp deltas without re-querying every event.
        Window focused = None;
        int revertTo = 0;
        XGetInputFocus(display, &focused, &revertTo);
        if (focused == None || focused == PointerRoot)
            focused = DefaultRootWindow(display);

        Window root, child;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (XQueryPointer(display, focused, &root, &child,
                          &rootX, &rootY, &winX, &winY, &mask))
        {
            g_virtualX = winX;
            g_virtualY = winY;
        }

        Screen* screen = DefaultScreenOfDisplay(display);
        g_screenW = WidthOfScreen(screen);
        g_screenH = HeightOfScreen(screen);

        grabbed = true;
        XFlush(display);
    }

    static void ungrabInput()
    {
        if (!grabbed)
            return;

        Display* display = (Display*)getKeyboardDisplay();
        if (!display)
            return;

        if (!grabbedDevices.empty())
        {
            for (int id : grabbedDevices)
                XIUngrabDevice(display, id, CurrentTime);
            grabbedDevices.clear();
        }
        else
        {
            XUngrabKeyboard(display, CurrentTime);
            XUngrabPointer(display, CurrentTime);
        }
        XFlush(display);

        g_buttonMask = 0;
        g_virtualX = 0;
        g_virtualY = 0;
        g_screenW = 0;
        g_screenH = 0;
        grabbed = false;
        Logger::info("input_blocker: input released");
    }

    void initInputBlocker(bool enabled)
    {
        blockingEnabled = enabled;

        if (!enabled && grabbed)
        {
            ungrabInput();
            blocked = false;
        }

        Logger::debug(std::string("Input blocking ") + (enabled ? "enabled" : "disabled"));
    }

    void setInputBlocked(bool shouldBlock)
    {
        if (!blockingEnabled)
        {
            Logger::info("input_blocker: setInputBlocked(" +
                         std::string(shouldBlock ? "true" : "false") +
                         ") ignored; blockingEnabled=false (overlayBlockInput config)");
            return;
        }

        if (shouldBlock == blocked)
            return;

        blocked = shouldBlock;

        Logger::info(std::string("input_blocker: ") +
                     (blocked ? "grabbing input" : "releasing input"));

        if (blocked)
            grabInput();
        else
            ungrabInput();
    }

    bool isInputBlocked()
    {
        return blocked;
    }

    void onRawMotion(void* xiRawEvent)
    {
        XIRawEvent* raw = static_cast<XIRawEvent*>(xiRawEvent);
        if (!grabbed || !raw)
            return;

        double dx = 0.0, dy = 0.0;
        if (raw->valuators.mask_len > 0)
        {
            // Axis 0 = X, axis 1 = Y for standard relative mice. Post-accel
            // values (valuators.values) match what natural cursor motion
            // would have been; raw_values is pre-accel. We want the overlay
            // cursor to feel identical to ungrabbed motion → use post-accel.
            int idx = 0;
            if (XIMaskIsSet(raw->valuators.mask, 0))
                dx = raw->valuators.values[idx++];
            if (XIMaskIsSet(raw->valuators.mask, 1))
                dy = raw->valuators.values[idx++];
        }

        if (dx == 0.0 && dy == 0.0)
            return;

        const int maxX = g_screenW > 0 ? g_screenW - 1 : 65535;
        const int maxY = g_screenH > 0 ? g_screenH - 1 : 65535;
        g_virtualX = std::clamp(g_virtualX + static_cast<int>(dx), 0, maxX);
        g_virtualY = std::clamp(g_virtualY + static_cast<int>(dy), 0, maxY);
    }

    void onRawButton(int button, bool pressed)
    {
        if (button < 0 || button >= 32)
            return;
        if (pressed)
            g_buttonMask |= (1u << button);
        else
            g_buttonMask &= ~(1u << button);
    }

    uint32_t currentButtonMask()
    {
        return g_buttonMask;
    }

    void virtualCursorPos(int& x, int& y)
    {
        x = g_virtualX;
        y = g_virtualY;
    }
}
