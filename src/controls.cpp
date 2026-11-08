#include "controls.h"

#include <fstream>
#include <sstream>

#include "common.h"

Controls controls;

namespace {

const char* const DEFAULT_INI = R"(# r6sv controls. Saved changes apply while the game runs.
#
# Actions: cross circle square triangle l1 r1 start select menu back
#          dpad_up dpad_down dpad_left dpad_right, or any Android keycode number.
#          Keyboard keys can also be move_up move_down move_left move_right.
# Keys use SDL key names (W, Space, Left Shift, Return, Escape...); controller
# buttons use SDL names (a b x y leftshoulder rightshoulder start back dpup...).

key.W = move_up
key.S = move_down
key.A = move_left
key.D = move_right
key.Up = dpad_up
key.Down = dpad_down
key.Left = dpad_left
key.Right = dpad_right
key.Space = cross
key.J = cross
key.K = circle
key.U = square
key.I = triangle
key.Q = l1
key.E = r1
key.Return = start
key.Tab = select
key.M = menu
key.Escape = back

button.a = cross
button.b = circle
button.x = square
button.y = triangle
button.leftshoulder = l1
button.rightshoulder = r1
button.start = start
button.back = back
button.dpup = dpad_up
button.dpdown = dpad_down
button.dpleft = dpad_left
button.dpright = dpad_right

trigger.left = l1
trigger.right = r1
trigger.threshold = 0.5

stick.deadzone = 0.15
look.stick_speed = 0.03
look.mouse_speed = 0.003
look.invert_y = 0

# Non-Xperia builds (Desire HD, Optimus 2X, Galaxy S, Droid X) have no gamepad
# mode, so movement and looking drive their on-screen controls instead. Settings
# under [touchscreen] only apply to those builds, [xperia] only to the Xperia
# Play build. Positions are in the game's 854x480 layout. Any key or button can
# also be bound to "touch X Y" to hold a finger on that spot of the screen.
[touchscreen]
touch.stick = 125 370
touch.stick_radius = 50
touch.look = 560 220
touch.look_range = 110
touch.look_dir = 1 1
# aiming only starts standing still, so the aim button stops you this many frames
touch.aim = 830 455
touch.aim_stop_frames = 8
# mouse clicks this close to the (hidden) stick are ignored in gameplay
touch.stick_block_radius = 110
# HUD buttons without a key: grenade, sprint (hold), aim down sights
button.y = touch 775 115
button.leftstick = touch 550 460
trigger.left = touch 830 455
key.G = touch 775 115
key.Left Shift = touch 550 460
key.F = touch 830 455
)";

int named_action(const std::string& v) {
    static const std::unordered_map<std::string, int> names = {
        {"cross", AK_CROSS}, {"circle", AK_CIRCLE}, {"square", AK_SQUARE}, {"triangle", AK_TRIANGLE},
        {"l1", AK_L1}, {"r1", AK_R1}, {"start", AK_START}, {"select", AK_SELECT}, {"menu", AK_MENU},
        {"back", AK_BACK}, {"dpad_up", AK_DPAD_UP}, {"dpad_down", AK_DPAD_DOWN},
        {"dpad_left", AK_DPAD_LEFT}, {"dpad_right", AK_DPAD_RIGHT}, {"move_up", MOVE_UP},
        {"move_down", MOVE_DOWN}, {"move_left", MOVE_LEFT}, {"move_right", MOVE_RIGHT}, {"none", 0},
    };
    auto it = names.find(v);
    if (it != names.end()) return it->second;
    char* end;
    long n = strtol(v.c_str(), &end, 10);
    return *end == 0 && !v.empty() ? (int)n : INT32_MIN;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "x y" -> two numbers
bool parse_pair(const std::string& v, float& a, float& b) {
    std::istringstream in(v);
    std::string rest;
    return (in >> a >> b) && !(in >> rest);
}

}  // namespace

int Controls::action(const std::string& v) {
    if (v.rfind("touch ", 0) == 0) {
        float x, y;
        if (!parse_pair(v.substr(6), x, y)) return INT32_MIN;
        touch_points.emplace_back(x, y);
        return TOUCH_ACTION + (int)touch_points.size() - 1;
    }
    return named_action(v);
}

void Controls::apply(std::istream& in, const std::string& where) {
    bool active = true;
    int n = 0;
    for (std::string line; std::getline(in, line);) {
        n++;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            std::string sec = line.substr(1, line.find(']') - 1);
            if (sec == "touchscreen") active = touchscreen;
            else if (sec == "xperia") active = !touchscreen;
            else if (sec == "all") active = true;
            else {
                active = false;
                LOGW("%s:%d: unknown section [%s]", where.c_str(), n, sec.c_str());
            }
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOGW("%s:%d: expected name = value", where.c_str(), n);
            continue;
        }
        if (!active) continue;
        std::string err = set(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
        if (!err.empty()) LOGW("%s:%d: %s", where.c_str(), n, err.c_str());
    }
}

void Controls::set_defaults() {
    bool ts = touchscreen;
    *this = Controls{};
    touchscreen = ts;
    std::istringstream in(DEFAULT_INI);
    apply(in, "defaults");
}

std::string Controls::set(const std::string& name, const std::string& value) {
    auto number = [&](float& out) -> std::string {
        char* end;
        float f = strtof(value.c_str(), &end);
        if (*end || value.empty()) return "not a number: " + value;
        out = f;
        return "";
    };
    if (name.rfind("key.", 0) == 0) {
        SDL_Keycode k = SDL_GetKeyFromName(name.substr(4).c_str());
        if (k == SDLK_UNKNOWN) return "unknown key: " + name.substr(4);
        int code = action(value);
        if (code == INT32_MIN) return "unknown action: " + value;
        if (code) keys[k] = code;
        else keys.erase(k);
        return "";
    }
    if (name.rfind("button.", 0) == 0) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.substr(7).c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return "unknown button: " + name.substr(7);
        int code = action(value);
        if (code == INT32_MIN || code < 0) return "unknown action: " + value;
        if (code) buttons[b] = code;
        else buttons.erase(b);
        return "";
    }
    if (name == "trigger.left" || name == "trigger.right") {
        int code = action(value);
        if (code == INT32_MIN || code < 0) return "unknown action: " + value;
        (name == "trigger.left" ? trigger_left : trigger_right) = code;
        return "";
    }
    auto pair = [&](float& a, float& b) -> std::string {
        return parse_pair(value, a, b) ? "" : "expected two numbers: " + value;
    };
    if (name == "touch.stick") return pair(touch.stick_x, touch.stick_y);
    if (name == "touch.stick_radius") return number(touch.stick_radius);
    if (name == "touch.look") return pair(touch.look_x, touch.look_y);
    if (name == "touch.look_range") return number(touch.look_range);
    if (name == "touch.look_dir") return pair(touch.look_dir_x, touch.look_dir_y);
    if (name == "touch.aim") return pair(touch.aim_x, touch.aim_y);
    if (name == "touch.aim_stop_frames") {
        float f = 0;
        std::string err = number(f);
        if (err.empty()) touch.aim_stop_frames = (int)f;
        return err;
    }
    if (name == "touch.stick_block_radius") return number(touch.stick_block_radius);
    if (name == "trigger.threshold") return number(trigger_threshold);
    if (name == "stick.deadzone") return number(stick_deadzone);
    if (name == "look.stick_speed") return number(look_stick_speed);
    if (name == "look.mouse_speed") return number(look_mouse_speed);
    if (name == "look.invert_y") {
        float f = 0;
        std::string err = number(f);
        look_invert_y = f != 0;
        return err;
    }
    return "unknown setting: " + name;
}

bool Controls::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    set_defaults();
    apply(in, path);
    LOGI("loaded %s (%s section)", path.c_str(), touchscreen ? "touchscreen" : "xperia");
    return true;
}

void Controls::write_default(const std::string& path) {
    std::ofstream out(path);
    out << DEFAULT_INI;
}
