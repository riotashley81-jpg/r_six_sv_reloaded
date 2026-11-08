// r6sv: runs the Android build of Rainbow Six: Shadow Vanguard on Windows by
// loading its native library into an ARM JIT and emulating the Java side.
//
// usage: r6sv <extracted apk dir> [data dir] [options]
//   --size WxH        render/window size (default 854x480)
//   --fullscreen      borderless fullscreen at desktop resolution
//   --frames N        quit after N frames
//   --fps N           frame limit (default 60, 0 = unlimited; the Android build ran at 30)
//   --shots a,b,...   save shot_<n>.bmp at those frame numbers
//   --headless        no window; GL calls go to a null renderer
//   --script file     scripted input, one "frame command args" per line:
//                       tap x y | down x y id | move x y id | up x y id |
//                       key code | keydown code | keyup code | shot | quit |
//                       lpad dx dy | rpad dx dy   (touchpad sticks, -1..1, lpad dy -1 = forward; 0 0 releases)
//                     touch coordinates are in 854x480 space, scaled to the render size;
//                     the game clock runs at a fixed 60 Hz per frame for repeatable runs
//   --console [port]  accept the same commands live on 127.0.0.1:port (default 47800),
//                     plus: +N <cmd> (run N frames later), wait N, status,
//                     set <setting> <value>, reload (see tools/r6ctl.py)
//   --controls file   input mapping (default controls.ini, created if missing,
//                     reloaded automatically when it changes)
#include <windows.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "console.h"
#include "controls.h"
#include "cpu.h"
#include "elf.h"
#include "glsl_to_cg.h"
#include "hle.h"
#include "jni.h"
#include "mem.h"
#include "platform.h"
#include "touchpad.h"

static std::mutex log_lock;

void log_line(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(log_lock);
    fprintf(stderr, "%s/%u: %s\n", tag, cpu::thread().tid, buf);
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line("F", "%s", buf);
    fflush(stderr);
    if (!hle::headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "r6sv", buf, nullptr);
    ExitProcess(1);
}

// --- platform -------------------------------------------------------------------
namespace platform {

std::string apk_dir;
JavaTouchpad touchpad;
static SDL_Window* window = nullptr;
static SDL_AudioDeviceID audio_dev = 0;
static u32 audio_bytes_per_sec = 0;
static u64 swaps = 0;

void swap_buffers() {
    swaps++;
    if (window) SDL_GL_SwapWindow(window);
}

void audio_open(u32 rate, u32 channels) {
    audio_bytes_per_sec = rate * channels * 2;
    if (hle::headless || audio_dev) return;
    SDL_AudioSpec want{}, got{};
    want.freq = (int)rate;
    want.format = AUDIO_S16LSB;
    want.channels = (u8)channels;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (!audio_dev) LOGW("audio: %s", SDL_GetError());
    else SDL_PauseAudioDevice(audio_dev, 0);
}

void audio_write(const void* pcm, u32 bytes) {
    if (!audio_dev) {
        // keep the game's audio thread at real-time speed
        if (audio_bytes_per_sec) Sleep((DWORD)((u64)bytes * 1000 / audio_bytes_per_sec));
        return;
    }
    // block like AudioTrack.write once ~100 ms is queued
    while (SDL_GetQueuedAudioSize(audio_dev) > audio_bytes_per_sec / 10) SDL_Delay(2);
    SDL_QueueAudio(audio_dev, pcm, bytes);
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == out.size();
}

}  // namespace platform

static std::string save_shot(int frame, int w, int h) {
    using ReadPixels = void(APIENTRY*)(int, int, int, int, u32, u32, void*);
    auto read = (ReadPixels)SDL_GL_GetProcAddress("glReadPixels");
    std::vector<u8> px((size_t)w * h * 4), flipped(px.size());
    read(0, 0, w, h, 0x1908 /*RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, px.data());
    for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    std::string name = "shot_" + std::to_string(frame) + ".bmp";
    SDL_SaveBMP(s, name.c_str());
    SDL_FreeSurface(s);
    LOGI("saved %s", name.c_str());
    char full[MAX_PATH];
    return GetFullPathNameA(name.c_str(), MAX_PATH, full, nullptr) ? full : name;
}

// --glsl2cg IN OUT: translate shader dumps (R6SV_GLTRACE writes IN/<n>.glsl) to
// Cg the way the Vita loader does, pairing <2k>.glsl (vertex) with <2k+1>.glsl.
static int glsl2cg(const std::string& in, const std::string& out) {
    CreateDirectoryA(out.c_str(), nullptr);
    int ok = 0, failed = 0;
    for (int n = 0;; n += 2) {
        std::vector<u8> vs, fs;
        if (!platform::read_file(in + "/" + std::to_string(n) + ".glsl", vs) ||
            !platform::read_file(in + "/" + std::to_string(n + 1) + ".glsl", fs))
            break;
        std::string v(vs.begin(), vs.end()), f(fs.begin(), fs.end());
        std::map<std::string, int> slots;
        for (const std::string& name : glsl::varyings(v, true)) slots.emplace(name, (int)slots.size());
        for (const std::string& name : glsl::varyings(f, false)) slots.emplace(name, (int)slots.size());
        for (int k = 0; k < 2; k++) {
            std::string cg, error;
            if (glsl::to_cg(k ? f : v, k == 0, slots, {}, cg, error)) {
                ok++;
            } else {
                failed++;
                cg = "// FAILED: " + error + "\n";
                fprintf(stderr, "%d.glsl: %s\n", n + k, error.c_str());
            }
            if (FILE* o = fopen((out + "/" + std::to_string(n + k) + ".cg").c_str(), "wb")) {
                fwrite(cg.data(), 1, cg.size(), o);
                fclose(o);
            }
        }
    }
    printf("translated %d shaders, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--glsl2cg") return glsl2cg(argv[2], argv[3]);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <extracted apk dir> [data dir] [--size WxH] [--fullscreen] [--frames N] "
                        "[--shots a,b] [--headless]\n", argv[0]);
        return 1;
    }
    platform::apk_dir = argv[1];
    std::string data_dir = "gamedata/GloftR6HP";
    int width = 854, height = 480, max_frames = -1, fps = 60;
    bool fullscreen = false;
    std::set<int> shots;
    std::multimap<int, std::string> script;
    int console_port = 0;
    std::string controls_path = "controls.ini";
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--frames" && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc) fps = atoi(argv[++i]);
        else if (a == "--headless") hle::headless = true;
        else if (a == "--shots" && i + 1 < argc) {
            for (char* t = strtok(argv[++i], ","); t; t = strtok(nullptr, ",")) shots.insert(atoi(t));
        } else if (a == "--script" && i + 1 < argc) {
            std::ifstream in(argv[++i]);
            if (!in) fatal("cannot open script %s", argv[i]);
            for (std::string line; std::getline(in, line);) {
                std::istringstream ls(line);
                int f;
                std::string rest;
                if (line.empty() || line[0] == '#' || !(ls >> f)) continue;
                std::getline(ls >> std::ws, rest);
                script.emplace(f, rest);
            }
        } else if (a == "--console") {
            console_port = 47800;
            if (i + 1 < argc && isdigit((u8)argv[i + 1][0])) console_port = atoi(argv[++i]);
        } else if (a == "--controls" && i + 1 < argc) {
            controls_path = argv[++i];
        } else data_dir = a;
    }
    hle::set_data_root(data_dir);

    SDL_GLContext gl = nullptr;
    if (!hle::headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL: %s", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        u32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        platform::window = SDL_CreateWindow("Rainbow Six: Shadow Vanguard", SDL_WINDOWPOS_CENTERED,
                                            SDL_WINDOWPOS_CENTERED, width, height, flags);
        if (!platform::window) fatal("SDL window: %s", SDL_GetError());
        gl = SDL_GL_CreateContext(platform::window);
        if (!gl) fatal("GL context: %s", SDL_GetError());
        SDL_GL_SetSwapInterval(1);
        SDL_GL_GetDrawableSize(platform::window, &width, &height);
    }

    mem::init();
    cpu::init();
    hle::init();
    cpu::thread_attach(1, 8 << 20);

    std::vector<u8> so;
    std::string so_path = platform::apk_dir + "/lib/armeabi-v7a/librainbowsix.so";
    if (!platform::read_file(so_path, so)) fatal("cannot read %s", so_path.c_str());
    Module game;
    if (!load_elf(so, mem::LIB_BASE, game)) fatal("failed to load %s", so_path.c_str());

    // scripted runs use a virtual 60 Hz clock so replays don't depend on speed;
    // set before any guest code reads the time
    if (!script.empty()) hle::virtual_clock_us = 1300000000ull * 1000000;

    java::init(game);
    LOGI("running %zu constructors", game.init_funcs.size());
    run_init(game);
    if (u32 onload = game.sym("JNI_OnLoad")) LOGI("JNI_OnLoad -> %08x", cpu::call(onload, {java::vm(), 0}));

    // Game.onCreate: report the Xperia Play's gamepad slider as open, which puts
    // the game in gamepad/touchpad mode instead of on-screen touch controls.
    // Other device builds lack that mode; their on-screen controls get driven.
    if (detect_xperia_touchpad()) java::call_native("Game", "nativeIsSlideEnable", {1});
    else hide_onscreen_stick(game);
    LOGI("input: %s", xperia_touchpad ? "Xperia Play rear touchpad" : "on-screen touch controls");

    // Same order as GameRenderer.onSurfaceCreated / onSurfaceChanged.
    LOGI("render size %dx%d", width, height);
    java::call_native("GameRenderer", "nativeInit");
    java::call_native("GLResLoader", "nativeInit");
    java::call_native("GLMediaPlayer", "nativeInit");
    java::call_native("GLUtils/Device", "nativeInit");
    java::call_native("Game", "nativeInit");
    java::call_native("GameRenderer", "nativeStart", {1, (u32)width, (u32)height, 0 /* language: English */});
    java::call_native("GameRenderer", "nativeResize", {(u32)width, (u32)height});

    controls.touchscreen = !xperia_touchpad;
    if (!std::filesystem::exists(controls_path)) Controls::write_default(controls_path);
    if (!controls.load(controls_path)) controls.set_defaults();
    std::error_code fs_err;
    auto controls_time = std::filesystem::last_write_time(controls_path, fs_err);
    if (console_port) console::start(console_port);

    TouchpadEmu pads;
    TouchscreenEmu screen;
    GameState state;
    state.init(game);
    SDL_GameController* pad = nullptr;
    float script_move[2] = {0, 0}, script_look[2] = {0, 0};
    int mouse_dx = 0, mouse_dy = 0;
    bool mouse_look = false;
    bool trig_down[2] = {false, false};
    auto key_event = [&](int code, bool down) {
        if (code >= TOUCH_ACTION) {
            size_t i = (size_t)(code - TOUCH_ACTION);
            if (i >= controls.touch_points.size()) return;
            screen_touch_flags();
            screen.hud((int)i, controls.touch_points[i].first, controls.touch_points[i].second, down);
            return;
        }
        java::call_native("Game", down ? "nativeSetOnKeyDown" : "nativeSetOnKeyUp", {(u32)code});
    };
    auto touch = [](const char* what, int x, int y) {
        screen_touch_flags();
        java::call_native("Game", what, {(u32)x, (u32)y, 0});
    };
    bool touching = false, running = true;
    auto t0 = std::chrono::steady_clock::now();
    auto next_frame = t0;
    auto fps_t = t0;
    int frame = 0, fps_frames = 0;
    double measured_fps = 0;

    // Commands shared by --script and --console; returns the reply text, or ""
    // when the reply is sent later (shot, wait).
    struct Deferred {
        int frame;
        std::promise<std::string> reply;
        bool shot;
    };
    std::vector<Deferred> deferred;
    bool shot = false;
    auto exec = [&](const std::string& line, std::promise<std::string>* reply) -> std::string {
        std::istringstream cmd(line);
        std::string op;
        int x = 0, y = 0, id = 0;
        cmd >> op;
        auto sx = [&](int v) { return v * width / 854; };
        auto sy = [&](int v) { return v * height / 480; };
        auto tch = [&](const char* what) {
            screen_touch_flags();
            java::call_native("Game", what, {(u32)sx(x), (u32)sy(y), (u32)id});
        };
        if (op.size() > 1 && op[0] == '+') {
            int at = frame + std::max(atoi(op.c_str() + 1), 1);
            std::string rest;
            std::getline(cmd >> std::ws, rest);
            script.emplace(at, rest);
            return "scheduled at frame " + std::to_string(at);
        }
        if (op == "tap") {
            cmd >> x >> y;
            tch("nativeTouchPressed");
            script.emplace(frame + 3, "up " + std::to_string(x) + " " + std::to_string(y) + " 0");
        } else if (op == "down" || op == "move" || op == "up") {
            cmd >> x >> y >> id;
            tch(op == "down" ? "nativeTouchPressed" : op == "move" ? "nativeTouchMoved" : "nativeTouchReleased");
        } else if (op == "key" || op == "keydown" || op == "keyup") {
            int code = 0;
            cmd >> code;
            key_event(code, op != "keyup");
            if (op == "key") script.emplace(frame + 30, "keyup " + std::to_string(code));  // the game polls keys
        } else if (op == "lpad" || op == "rpad") {
            float dx = 0, dy = 0;
            cmd >> dx >> dy;
            float* s = op == "lpad" ? script_move : script_look;
            s[0] = dx;
            s[1] = dy;
        } else if (op == "shot") {
            shot = true;
            if (reply) {
                deferred.push_back({frame, std::move(*reply), true});
                return "";
            }
        } else if (op == "wait") {
            int n = 0;
            cmd >> n;
            if (reply) {
                deferred.push_back({frame + std::max(n, 0), std::move(*reply), false});
                return "";
            }
        } else if (op == "status") {
            auto& tp = platform::touchpad;
            char buf[256];
            snprintf(buf, sizeof buf, "frame=%d fps=%.1f size=%dx%d input=%s move=(%.2f,%.2f) look=(%.2f,%.2f) M,N=%d,%d",
                     frame, measured_fps, width, height, xperia_touchpad ? "touchpad" : "touchscreen",
                     script_move[0], script_move[1], script_look[0], script_look[1], tp.M, tp.N);
            return buf;
        } else if (op == "set") {
            std::string name, value;
            cmd >> name;
            std::getline(cmd >> std::ws, value);
            std::string err = controls.set(name, value);
            return err.empty() ? "ok" : "error: " + err;
        } else if (op == "reload") {
            return controls.load(controls_path) ? "ok" : "error: cannot read " + controls_path;
        } else if (op == "quit") {
            running = false;
        } else {
            return "error: unknown command: " + op;
        }
        return "ok";
    };

    for (; running && (max_frames < 0 || frame < max_frames); frame++) {
        SDL_Event ev;
        while (!hle::headless && SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (ev.key.repeat) break;
                if (ev.key.keysym.sym == SDLK_F11 && ev.type == SDL_KEYDOWN) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(platform::window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                }
                auto it = controls.keys.find(ev.key.keysym.sym);
                if (it != controls.keys.end() && it->second > 0) key_event(it->second, ev.type == SDL_KEYDOWN);
                break;
            }
            case SDL_CONTROLLERDEVICEADDED:
                if (!pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                    SDL_GameControllerClose(pad);
                    pad = nullptr;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                auto it = controls.buttons.find(ev.cbutton.button);
                if (it != controls.buttons.end()) key_event(it->second, ev.type == SDL_CONTROLLERBUTTONDOWN);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    mouse_look = true;
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
                if (ev.button.button == SDL_BUTTON_LEFT && !screen.blocks_touch(ev.button.x, ev.button.y)) { touching = true; touch("nativeTouchPressed", ev.button.x, ev.button.y); }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    mouse_look = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
                if (ev.button.button == SDL_BUTTON_LEFT && touching) { touching = false; touch("nativeTouchReleased", ev.button.x, ev.button.y); }
                break;
            case SDL_MOUSEMOTION:
                if (mouse_look) {
                    mouse_dx += ev.motion.xrel;
                    mouse_dy += ev.motion.yrel;
                    break;
                }
                if (touching) touch("nativeTouchMoved", ev.motion.x, ev.motion.y);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GL_GetDrawableSize(platform::window, &width, &height);
                    LOGI("resize %dx%d", width, height);
                    java::call_native("GameRenderer", "nativeResize", {(u32)width, (u32)height});
                }
                break;
            }
        }

        // controls.ini edited? (checked twice a second)
        if (frame % 30 == 0) {
            auto t = std::filesystem::last_write_time(controls_path, fs_err);
            if (!fs_err && t != controls_time) {
                controls_time = t;
                controls.load(controls_path);
            }
        }

        shot = shots.count(frame) > 0;
        for (auto [it, end] = script.equal_range(frame); it != end; ++it) exec(it->second, nullptr);
        for (auto& req : console::take()) {
            std::string r = exec(req->line, &req->reply);
            if (!r.empty()) req->reply.set_value(r);
        }

        // sticks / keyboard / mouse -> rear touchpad, or on-screen controls
        {
            screen.layout = controls.touch;
            screen.width = width;
            screen.height = height;
            if (!xperia_touchpad) screen.set_active(state.playing());
            auto axis = [&](SDL_GameControllerAxis a) {
                float v = pad ? SDL_GameControllerGetAxis(pad, a) / 32767.f : 0.f;
                return std::fabs(v) < controls.stick_deadzone ? 0.f : std::clamp(v, -1.f, 1.f);
            };
            float mx = script_move[0], my = script_move[1];
            if (mx == 0 && my == 0) {
                mx = axis(SDL_CONTROLLER_AXIS_LEFTX);
                my = axis(SDL_CONTROLLER_AXIS_LEFTY);
            }
            if (mx == 0 && my == 0 && !hle::headless) {
                const u8* ks = SDL_GetKeyboardState(nullptr);
                for (auto& [key, code] : controls.keys) {
                    if (code >= 0 || !ks[SDL_GetScancodeFromKey(key)]) continue;
                    if (code == MOVE_UP) my -= 1;
                    if (code == MOVE_DOWN) my += 1;
                    if (code == MOVE_LEFT) mx -= 1;
                    if (code == MOVE_RIGHT) mx += 1;
                }
            }
            // touchpad y runs bottom-up: W / stick up moves forward
            if (xperia_touchpad) pads.stick(0, mx, -my);
            else screen.stick(0, mx, -my);

            float lx = (script_look[0] + axis(SDL_CONTROLLER_AXIS_RIGHTX)) * controls.look_stick_speed +
                       mouse_dx * controls.look_mouse_speed;
            float ly = (script_look[1] + axis(SDL_CONTROLLER_AXIS_RIGHTY)) * controls.look_stick_speed +
                       mouse_dy * controls.look_mouse_speed;
            if (controls.look_invert_y) ly = -ly;
            if (xperia_touchpad) pads.look(lx, -ly, mouse_look);  // touchpad y runs bottom-up
            else screen.look(lx, -ly, mouse_look);
            mouse_dx = mouse_dy = 0;

            for (int i = 0; i < 2; i++) {
                bool down = axis(i ? SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_AXIS_TRIGGERLEFT) >
                            controls.trigger_threshold;
                int code = i ? controls.trigger_right : controls.trigger_left;
                if (down != trig_down[i]) {
                    trig_down[i] = down;
                    if (code) key_event(code, down);
                }
            }
        }

        if (hle::virtual_clock_us) hle::virtual_clock_us += 1000000 / 60;
        // R6SV_TRACEFRAME=N: log every GL call made while rendering frame N
        static const int trace_frame = getenv("R6SV_TRACEFRAME") ? atoi(getenv("R6SV_TRACEFRAME")) : -1;
        cpu::trace_gl_calls = frame == trace_frame;
        java::call_native("GameRenderer", "nativeRender");
        cpu::trace_gl_calls = false;
        std::string shot_path;
        if (!hle::headless && shot) shot_path = save_shot(frame, width, height);
        for (auto it = deferred.begin(); it != deferred.end();) {
            if (it->frame > frame) {
                ++it;
                continue;
            }
            it->reply.set_value(it->shot ? (shot_path.empty() ? "error: no window" : shot_path)
                                         : "frame " + std::to_string(frame));
            it = deferred.erase(it);
        }
        platform::swap_buffers();  // GLSurfaceView swaps after every onDrawFrame
        if (fps > 0) {
            next_frame += std::chrono::microseconds(1000000 / fps);
            auto now = std::chrono::steady_clock::now();
            if (next_frame > now) std::this_thread::sleep_until(next_frame);
            else next_frame = now;  // running behind: don't try to catch up
        }
        if (++fps_frames == 60) {
            auto now = std::chrono::steady_clock::now();
            measured_fps = 60 / std::chrono::duration<double>(now - fps_t).count();
            fps_t = now;
            fps_frames = 0;
        }
        if (frame % 300 == 0) LOGI("frame %d", frame);
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("%d frames in %.2fs (%.1f fps)", frame, secs, frame / secs);
    fflush(stderr);
    ExitProcess(0);
}
