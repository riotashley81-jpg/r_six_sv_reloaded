// Xperia Play rear touchpad emulation, shared by the PC and Vita frontends.
#pragma once
#include <algorithm>
#include <cmath>

#include "cpu.h"
#include "elf.h"
#include "jni.h"
#include "mem.h"
#include "platform.h"

// The Xperia Play's rear touchpad (966x360, two pads) is how this build
// moves and looks. Game.onTouchEvent flags the touch as touchpad, marks which
// pad each finger is on, and maps pad coordinates through Game.tx/ty before
// sending ordinary touch events. Analog sticks impersonate a finger held on
// a pad, deflected from its centre.
struct TouchpadEmu {
    struct Pad {
        bool down = false;
        float x = 0, y = 0;  // deflection, -1..1
    } pad[2];                // 0 = left (move), 1 = right (look)
    static constexpr u32 POINTER_ID[2] = {5, 6};

    // Game.tx / Game.ty from the Java code, on raw touchpad coordinates
    static int tx(int x) {
        if (x < 380) return x > 190 ? (int)((x - 190) / 2.0) + 124 : 124 - (int)((190 - x) / 2.0);
        return x > 770 ? (int)((x - 770) / 2.3) + 120 : 120 - (int)((770 - x) / 2.3);
    }
    static int ty(int x, int y) {
        if (x < 380) return y > 190 ? 350 - (int)((y - 190) / 2.0) : (int)((190 - y) / 2.0) + 350;
        return y > 190 ? 170 - (int)((y - 190) / 3.0) : (int)((190 - y) / 3.0) + 170;
    }
    static void raw(int i, const Pad& p, int& x, int& y) {
        const int cx = i == 0 ? 190 : 770;
        x = cx + (int)(p.x * 180);
        y = 190 + (int)(p.y * 170);
    }

    void flags() {
        java::call_native("Game", "nativeIsTouchPad", {1});
        java::call_native("Game", "nativeIsRightTouchPad", {(u32)-1});
        java::call_native("Game", "nativeIsLeftTouchPad", {(u32)-1});
        if (pad[1].down) java::call_native("Game", "nativeIsRightTouchPad", {1});
        if (pad[0].down) java::call_native("Game", "nativeIsLeftTouchPad", {1});
    }
    void send(int i, const char* what) {
        int x, y;
        raw(i, pad[i], x, y);
        java::call_native("Game", what, {(u32)tx(x), (u32)ty(x, y), POINTER_ID[i]});
    }

    // mirror Game.onTouchEvent's bookkeeping of the static touchpad fields
    void update_java(int i, bool down) {
        auto& t = platform::touchpad;
        int x, y;
        raw(i, pad[i], x, y);
        if (i == 0) {
            t.M = down ? x : -1;
            t.N = down ? y : -1;
        } else if (down) {
            t.Q = x - t.C;
            t.R = -(y - t.D);
            t.O = x - 770;
            t.P = -(y - 190);
        } else {
            t.O = t.P = t.Q = t.R = 0;
        }
    }

    void press(int i) {
        pad[i] = {true, 0, 0};
        if (i == 1) {
            int x, y;
            raw(1, pad[1], x, y);
            platform::touchpad.C = x;
            platform::touchpad.D = y;
        }
        update_java(i, true);
        flags();
        send(i, "nativeTouchPressed");
    }
    void lift(int i) {
        flags();
        send(i, "nativeTouchReleased");
        pad[i].down = false;
        update_java(i, false);
        flags();
    }

    // Left pad (move): hold a finger at the stick's deflection; inside the
    // deadzone the finger lifts.
    void stick(int i, float x, float y) {
        Pad& p = pad[i];
        bool active = x * x + y * y > 0.0025f;
        if (!active) {
            if (p.down) lift(i);
            return;
        }
        if (!p.down) press(i);
        x = std::clamp(x, -1.f, 1.f);
        y = std::clamp(y, -1.f, 1.f);
        if (p.x == x && p.y == y) return;
        p.x = x;
        p.y = y;
        update_java(i, true);
        flags();
        send(i, "nativeTouchMoved");
    }

    // Right pad (look): the camera follows the finger's motion, so slide the
    // finger by (dx, dy) each frame and recentre at the pad's edge like a
    // thumb would. With hold, the finger stays down while idle (mouse look).
    void look(float dx, float dy, bool hold) {
        Pad& p = pad[1];
        if (dx == 0 && dy == 0) {
            if (p.down && !hold) lift(1);
            return;
        }
        if (!p.down) press(1);
        float nx = p.x + dx, ny = p.y + dy;
        if (std::fabs(nx) > 1 || std::fabs(ny) > 1) {
            lift(1);
            press(1);
            nx = std::clamp(dx, -1.f, 1.f);
            ny = std::clamp(dy, -1.f, 1.f);
        }
        p.x = nx;
        p.y = ny;
        update_java(1, true);
        flags();
        send(1, "nativeTouchMoved");
    }
};

// Only the Xperia Play build has the touchpad natives; the other device builds
// (Desire HD, Optimus 2X, Galaxy S, Droid X) play with on-screen controls.
inline bool xperia_touchpad = true;
inline bool detect_xperia_touchpad() {
    return xperia_touchpad = java::has_native("Game", "nativeIsTouchPad");
}

// Where the on-screen controls are, in the game's 854x480 layout space.
struct TouchLayout {
    float stick_x = 125, stick_y = 370, stick_radius = 50;  // virtual move stick
    float look_x = 560, look_y = 220, look_range = 110;     // empty area dragged to look
    // finger direction per look direction (the camera follows the finger)
    float look_dir_x = 1, look_dir_y = 1;
    // Aim down sights acts when its button is released, and only if the
    // player is standing then (otherwise it waits until they stop). So the
    // stick is released while the aim button is held and this long after.
    float aim_x = 830, aim_y = 455;
    int aim_stop_frames = 8;
    // Real touches inside this circle around the stick are dropped during
    // gameplay: the stick is hidden, so the left of the screen doesn't move.
    float stick_block_radius = 110;
};

// On-screen controls emulation for the non-Xperia builds, with the same stick /
// look interface as TouchpadEmu: the left stick holds a finger on the virtual
// stick, looking drags a finger across an empty part of the screen, and HUD
// buttons get a finger of their own while held.
//
// The fingers only go down during gameplay: in menus the same spots hold menu
// buttons (the sprint spot is "More Games" on the main menu), and a finger
// held there steals the menu's focus from the buttons and D-pad.
struct TouchscreenEmu {
    TouchLayout layout;
    int width = 854, height = 480;  // render size
    static constexpr u32 STICK_ID = 5, LOOK_ID = 6, HUD_ID = 10;
    static constexpr int MAX_HUD = 32;
    struct Finger {
        bool down = false;
        float x = 0, y = 0;  // layout-space position
    } stick_f, look_f, hud_f[MAX_HUD];
    bool active = true;  // in gameplay (see GameState)
    int stick_paused = 0;  // frames left with the stick released (aiming)
    int aim_held = 0;      // aim button fingers down

    // Leaving gameplay lifts every finger; bound inputs still held when it
    // resumes stay up until pressed again.
    void set_active(bool a) {
        if (a == active) return;
        active = a;
        LOGI("on-screen controls %s", a ? "on (gameplay)" : "off (menu)");
        if (a) return;
        lift(stick_f, STICK_ID);
        lift(look_f, LOOK_ID);
        for (int i = 0; i < MAX_HUD; i++) lift(hud_f[i], HUD_ID + (u32)i);
        aim_held = stick_paused = 0;
    }

    void send(const char* what, float x, float y, u32 id) const {
        java::call_native("Game", what, {(u32)(x * width / 854), (u32)(y * height / 480), id});
    }
    void press(Finger& f, float x, float y, u32 id) {
        f = {true, x, y};
        send("nativeTouchPressed", x, y, id);
    }
    void move(Finger& f, float x, float y, u32 id) {
        if (f.x == x && f.y == y) return;
        f.x = x;
        f.y = y;
        send("nativeTouchMoved", x, y, id);
    }
    void lift(Finger& f, u32 id) {
        if (!f.down) return;
        send("nativeTouchReleased", f.x, f.y, id);
        f.down = false;
    }

    // x right, y forward, -1..1; inside the deadzone the finger lifts
    void stick(int, float x, float y) {
        const auto& L = layout;
        if (aim_held > 0) return lift(stick_f, STICK_ID);
        if (stick_paused > 0) {
            stick_paused--;
            return lift(stick_f, STICK_ID);
        }
        if (!active || x * x + y * y <= 0.0025f) return lift(stick_f, STICK_ID);
        // the game only sees a frame's last touch position, so the press needs
        // a frame of its own on the knob before the finger moves
        if (!stick_f.down) return press(stick_f, L.stick_x, L.stick_y, STICK_ID);
        x = std::clamp(x, -1.f, 1.f);
        y = std::clamp(y, -1.f, 1.f);
        move(stick_f, L.stick_x + x * L.stick_radius, L.stick_y - y * L.stick_radius, STICK_ID);
    }

    // (dx, dy) in touchpad widths, y up, like TouchpadEmu::look; the finger
    // slides by that much and recentres at the edge of the look area
    void look(float dx, float dy, bool hold) {
        const auto& L = layout;
        if (!active) return lift(look_f, LOOK_ID);
        if (dx == 0 && dy == 0) {
            if (!hold) lift(look_f, LOOK_ID);
            return;
        }
        if (!look_f.down) return press(look_f, L.look_x, L.look_y, LOOK_ID);  // see stick()
        float ddx = dx * L.look_range * L.look_dir_x, ddy = -dy * L.look_range * L.look_dir_y;
        float nx = look_f.x + ddx, ny = look_f.y + ddy;
        if (std::fabs(nx - L.look_x) > L.look_range || std::fabs(ny - L.look_y) > L.look_range) {
            // thumb off the edge: lift now, put it back at the centre next frame
            lift(look_f, LOOK_ID);
            return;
        }
        move(look_f, nx, ny, LOOK_ID);
    }

    // hold a finger on a HUD point (layout space) while a bound input is down
    void hud(int slot, float x, float y, bool down) {
        if (slot < 0 || slot >= MAX_HUD) return;
        Finger& f = hud_f[slot];
        bool aim = std::hypot(x - layout.aim_x, y - layout.aim_y) < 30;
        if (down && active && !f.down) {
            press(f, x, y, HUD_ID + (u32)slot);
            if (aim) aim_held++;
        } else if (!down && f.down) {
            lift(f, HUD_ID + (u32)slot);
            if (aim && aim_held > 0) {
                aim_held--;
                stick_paused = layout.aim_stop_frames;
            }
        }
    }

    // whether a real touch at (x, y) in render pixels should be dropped
    bool blocks_touch(int x, int y) const {
        if (!active) return false;
        float lx = x * 854.f / width - layout.stick_x, ly = y * 480.f / height - layout.stick_y;
        return std::hypot(lx, ly) < layout.stick_block_radius;
    }
};

// The non-Xperia builds draw their virtual stick on the left of the screen.
// The controller drives it, so hide it: the draw entries of the stick's ring
// and knob classes' vtables (RenderSelf, RenderChildren) are pointed at an
// empty function. The stick still takes touches. The Thumbstick container
// class is left alone: the look area is one too, and the crosshair is drawn
// as its child.
inline void hide_onscreen_stick(const Module& m) {
    u32 noop = m.sym("_ZN11MenuControl10RenderTextEPKcRKNS_7ContextE");
    if (!noop || (mem::rd32(noop & ~1u) & 0xffff) != 0x4770) {  // bx lr
        LOGW("cannot hide the on-screen stick: no empty function found");
        return;
    }
    int n = 0;
    for (const char* vt : {"_ZTV14ThumbstickBase", "_ZTV13ThumbstickTip"}) {
        u32 v = m.sym(vt);
        if (!v) continue;
        mem::wr32(v + 0x54, noop);  // RenderSelf
        mem::wr32(v + 0x58, noop);  // RenderChildren
        n++;
    }
    LOGI("on-screen stick hidden (%d classes)", n);
}

// Whether a mission is being played, rather than a menu, briefing or the pause
// menu: Gameplay::GetWorld() and Gameplay::GetHud() exist and !Hud::IsPaused().
// These are plain getters in every device build.
struct GameState {
    u32 get_world = 0, get_hud = 0, is_paused = 0;
    void init(const Module& m) {
        get_world = m.sym("_ZN8Gameplay8GetWorldEv");
        get_hud = m.sym("_ZN8Gameplay6GetHudEv");
        is_paused = m.sym("_ZNK3Hud8IsPausedEv");
        if (!get_world || !get_hud || !is_paused) LOGW("game state functions not found; HUD controls stay on in menus");
    }
    bool playing() const {
        if (!get_world || !get_hud || !is_paused) return true;
        if (!cpu::call(get_world)) return false;
        u32 hud = cpu::call(get_hud);
        return hud && !cpu::call(is_paused, {hud});
    }
};

// before a screen touch, undo the touchpad flags like the Java code does
inline void screen_touch_flags() {
    if (!xperia_touchpad) return;
    java::call_native("Game", "nativeIsRightTouchPad", {(u32)-1});
    java::call_native("Game", "nativeIsLeftTouchPad", {(u32)-1});
    java::call_native("Game", "nativeIsTouchPad", {0});
}
