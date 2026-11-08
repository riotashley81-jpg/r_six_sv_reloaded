// Input mapping, loaded from controls.ini and reloaded when the file changes.
#pragma once
#include <SDL.h>

#include <istream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "android_keys.h"
#include "touchpad.h"

// Action codes from TOUCH_ACTION up hold a finger on touch_points[code - TOUCH_ACTION].
inline constexpr int TOUCH_ACTION = 0x100000;

struct Controls {
    std::unordered_map<SDL_Keycode, int> keys;  // keyboard -> Android keycode, MoveDir or touch action
    std::unordered_map<int, int> buttons;       // SDL_GameControllerButton -> Android keycode or touch action
    int trigger_left = AK_L1, trigger_right = AK_R1;
    float trigger_threshold = 0.5f;
    float stick_deadzone = 0.15f;
    float look_stick_speed = 0.03f;   // touchpad widths per frame at full deflection
    float look_mouse_speed = 0.003f;  // touchpad widths per mouse pixel
    bool look_invert_y = false;
    TouchLayout touch;                                   // on-screen controls (non-Xperia builds)
    std::vector<std::pair<float, float>> touch_points;  // targets of "touch X Y" actions

    // Which backend's [section] applies: [touchscreen] when true, [xperia] when false.
    bool touchscreen = false;

    void set_defaults();
    // Apply one "name = value" setting; returns an error message or "".
    std::string set(const std::string& name, const std::string& value);
    bool load(const std::string& path);
    static void write_default(const std::string& path);

private:
    int action(const std::string& v);
    void apply(std::istream& in, const std::string& where);
};

extern Controls controls;
