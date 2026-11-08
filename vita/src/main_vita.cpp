// r6sv for PS Vita: loads the Android build's librainbowsix.so natively and
// emulates the Java side, like the PC loader minus the JIT.
//
// Files (see vita/README.md):
//   ux0:data/r6sv/apk/        extracted APK (lib/armeabi-v7a/librainbowsix.so, res/, assets/)
//   ux0:data/r6sv/gamedata/   the game's data (what Android keeps in /sdcard/gameloft/games/GloftR6HP)
//   ux0:data/r6sv/log.txt     log of the last run (crashes are reported here)
//
// Needs kubridge.skprx and an extracted libshacccg.suprx.
#include <kubridge.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <vitaGL.h>

#include <pthread.h>

#include <malloc.h>

#include <cstdarg>
#include <exception>
#include <typeinfo>
#include <mutex>
#include <vector>

#include "android_keys.h"
#include "cpu.h"
#include "elf.h"
#include "hle.h"
#include "jni.h"
#include "mem.h"
#include "native_direct.h"
#include "os.h"
#include "platform.h"
#include "touchpad.h"
#include "vita_screen.h"

int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;  // the game runs on this thread

static const char* const ROOT = "ux0:data/r6sv/";

// --- logging --------------------------------------------------------------------
static std::mutex log_lock;
static FILE* log_file = nullptr;
static bool gl_ready = false;

void log_line(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lk(log_lock);
    sceClibPrintf("%s/%u: %s\n", tag, cpu::thread().tid, buf);
    if (log_file) {
        fprintf(log_file, "%s/%u: %s\n", tag, cpu::thread().tid, buf);
        fflush(log_file);  // so the log survives a crash
    }
}

static void show_error(const char* text) {
    if (!gl_ready) {
        vglInit(0);
        gl_ready = true;
    }
    SceMsgDialogUserMessageParam msg{};
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = (const SceChar8*)text;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_line("F", "%s", buf);
    show_error(buf);
    os::exit_process(1);
}

// An uncaught C++ exception in the loader (a failed allocation, a mutex the
// system couldn't create) ends in abort(); say which one first.
static void on_terminate() {
    const char* what = "no active exception";
    const char* type = "";
    char buf[160];
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            type = typeid(x).name();
            snprintf(buf, sizeof buf, "%s", x.what());
            what = buf;
        } catch (...) {
            what = "unknown exception";
        }
    }
    struct mallinfo mi = mallinfo();
    log_line("F", "terminate: %s %s (heap %u KB in use, %u KB reserved of %u KB)", type, what, (u32)mi.uordblks >> 10,
             (u32)mi.arena >> 10, (u32)_newlib_heap_size_user >> 10);
    abort();
}

// Crash report: registers, with addresses inside the game library given as
// offsets so they can be looked up in librainbowsix.so.
static u32 lib_base = 0, lib_size = 0;
static const u32 HANDLED_EXCEPTIONS[] = {KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
                                         KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION};
static void exception_handler(KuKernelExceptionContext* c) {
    // A fault inside the report itself must not re-enter it: that recursed down
    // the stack, reporting forever, and looked like a frozen loading screen.
    static volatile int active = 0;
    if (active++) {
        for (u32 t : HANDLED_EXCEPTIONS) kuKernelReleaseExceptionHandler(t);
        return;
    }
    auto where = [](u32 a) {
        static char s[4][32];
        static int k;
        char* b = s[k++ & 3];
        if (a >= lib_base && a < lib_base + lib_size) snprintf(b, 32, "%08x (lib+%06x)", a, a - lib_base);
        else snprintf(b, 32, "%08x", a);
        return b;
    };
    static const char* const kinds[] = {"data abort", "prefetch abort", "undefined instruction"};
    log_line("F", "CRASH (%s) pc=%s lr=%s sp=%08x far=%08x", c->exceptionType < 3 ? kinds[c->exceptionType] : "?",
             where(c->pc), where(c->lr), c->sp, c->FAR);
    log_line("F", "r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x", c->r0, c->r1, c->r2, c->r3, c->r4, c->r5, c->r6);
    log_line("F", "r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x", c->r7, c->r8, c->r9, c->r10, c->r11, c->r12);
    log_line("F", "last import called on this thread: %s", cpu::thunk_name(cpu::last_svc()));
    {
        struct mallinfo mi = mallinfo();
        log_line("F", "heap: %u KB in use, %u KB reserved of %u KB", (u32)mi.uordblks >> 10, (u32)mi.arena >> 10,
                 (u32)_newlib_heap_size_user >> 10);
    }
    // likely return addresses: odd (Thumb) words pointing into the game library
    char line[256];
    int n = 0, len = 0;
    for (u32 a = c->sp & ~3u; a < (c->sp & ~3u) + 4096 && n < 24; a += 4) {
        // the stack's top may be just above sp; stop at the first unmapped page
        if ((a == (c->sp & ~3u) || (a & 0xfff) == 0) && sceKernelFindMemBlockByAddr((void*)(uintptr_t)a, 4) < 0) break;
        u32 v = *(u32*)(uintptr_t)a;
        if ((v & 1) && v >= lib_base && v < lib_base + lib_size) {
            len += snprintf(line + len, sizeof line - len, " lib+%06x", v - 1 - lib_base);
            if (++n % 8 == 0) {
                log_line("F", "stack:%s", line);
                len = 0;
            }
        }
    }
    if (len) log_line("F", "stack:%s", line);
    // memory that registers point to (only mapped memory is read)
    u32 regs[13] = {c->r0, c->r1, c->r2, c->r3, c->r4, c->r5, c->r6, c->r7, c->r8, c->r9, c->r10, c->r11, c->r12};
    for (int i = 0; i < 13; i++) {
        u32 a = regs[i] & ~3u;
        if (a < 0x81000000 || a >= 0xA0000000) continue;
        if (sceKernelFindMemBlockByAddr((void*)(uintptr_t)a, 64) < 0) continue;
        len = 0;
        for (int k = 0; k < 16; k++) len += snprintf(line + len, sizeof line - len, " %08x", ((u32*)(uintptr_t)a)[k]);
        log_line("F", "[r%d]%s", i, line);
    }
    log_line("F", "recent file operations (op guestFILE a b -> result name):");
    hle::dump_file_ops([](const char* s) { log_line("F", "  %s", s); });
    // Exiting from inside the handler freezes the console. Step aside instead:
    // the faulting instruction runs again and the system closes the app as usual.
    for (u32 t : HANDLED_EXCEPTIONS) kuKernelReleaseExceptionHandler(t);
}

// --- platform ---------------------------------------------------------------------
namespace platform {

std::string apk_dir;
JavaTouchpad touchpad;

void swap_buffers() { vglSwapBuffers(GL_FALSE); }

// AudioTrack.write hands over arbitrary sizes; sceAudioOut wants whole grains
// and blocks until each is queued, which paces the game's audio thread.
static constexpr int GRAIN = 1024;  // samples per channel
static int audio_port = -1;
static u32 audio_channels = 2;
static std::vector<u8> audio_pending;

void audio_open(u32 rate, u32 channels) {
    if (audio_port >= 0) return;
    audio_channels = channels;
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, (int)rate,
                                     channels == 2 ? SCE_AUDIO_OUT_MODE_STEREO : SCE_AUDIO_OUT_MODE_MONO);
    if (audio_port < 0) {
        LOGW("audio: cannot open %u Hz x%u port (0x%08x)", rate, channels, (u32)audio_port);
        return;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(audio_port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
}

void audio_write(const void* pcm, u32 bytes) {
    if (audio_port < 0) return;
    const u32 grain_bytes = GRAIN * audio_channels * 2;
    audio_pending.insert(audio_pending.end(), (const u8*)pcm, (const u8*)pcm + bytes);
    size_t off = 0;
    for (; audio_pending.size() - off >= grain_bytes; off += grain_bytes)
        sceAudioOutOutput(audio_port, audio_pending.data() + off);
    audio_pending.erase(audio_pending.begin(), audio_pending.begin() + off);
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

void hle::init_native_direct() {
    for (const NativeDirect* d = native_direct; d->name; d++) reg_data(d->name, (u32)(uintptr_t)d->fn);
}

// Every per-thread variable in the loader depends on this (see CMakeLists.txt).
static void check_threading() {
    static thread_local int probe = 0;
    probe = 1;
    int seen = -1;
    pthread_t t;
    pthread_create(&t, nullptr, [](void* out) -> void* { *(int*)out = probe; probe = 2; return nullptr; }, &seen);
    pthread_join(t, nullptr);
    if (seen != 0 || probe != 1) fatal("thread_local is shared between threads (saw %d, then %d); build is broken", seen, probe);
    LOGI("threading OK");
}

static bool exists(const std::string& path) {
    SceIoStat st;
    return sceIoGetstat(path.c_str(), &st) >= 0;
}

// --- input ------------------------------------------------------------------------
// Vita buttons -> the Xperia Play's gamepad keys. SELECT is the phone's BACK
// key, which opens the pause menu.
static const struct { u32 button; int key; } BUTTONS[] = {
    {SCE_CTRL_CROSS, AK_CROSS},       {SCE_CTRL_CIRCLE, AK_CIRCLE},     {SCE_CTRL_SQUARE, AK_SQUARE},
    {SCE_CTRL_TRIANGLE, AK_TRIANGLE}, {SCE_CTRL_LTRIGGER, AK_L1},       {SCE_CTRL_RTRIGGER, AK_R1},
    {SCE_CTRL_START, AK_START},       {SCE_CTRL_SELECT, AK_BACK},       {SCE_CTRL_UP, AK_DPAD_UP},
    {SCE_CTRL_DOWN, AK_DPAD_DOWN},    {SCE_CTRL_LEFT, AK_DPAD_LEFT},    {SCE_CTRL_RIGHT, AK_DPAD_RIGHT},
};
// The other device builds (Desire HD, Optimus 2X, Galaxy S, Droid X) ignore
// Triangle and L1 and have HUD buttons with no key at all, so in their
// touchscreen mode these also hold a finger on a HUD button (854x480 layout,
// as on PC). Circle keeps its own action and sprints while held.
static const struct { u32 button; float x, y; } HUD_BUTTONS[] = {
    {SCE_CTRL_TRIANGLE, 775, 115},  // grenade
    {SCE_CTRL_LTRIGGER, 830, 455},  // aim down sights
    {SCE_CTRL_CIRCLE, 550, 460},    // sprint
};
constexpr int HUD_BUTTON_COUNT = (int)(sizeof HUD_BUTTONS / sizeof HUD_BUTTONS[0]);
constexpr float STICK_DEADZONE = 0.15f;
constexpr float LOOK_SPEED = 0.03f;  // touchpad widths per frame at full deflection, as on PC

static float stick_axis(u8 v) {
    float f = ((int)v - 128) / 127.f;
    return std::fabs(f) < STICK_DEADZONE ? 0.f : std::clamp(f, -1.f, 1.f);
}

int main() {
    sceIoMkdir("ux0:data/r6sv", 0777);
    log_file = fopen((std::string(ROOT) + "log.txt").c_str(), "w");
    LOGI("r6sv for PS Vita starting");
    check_threading();

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    platform::apk_dir = std::string(ROOT) + "apk";
    hle::set_data_root(std::string(ROOT) + "gamedata");
    std::string so_path = platform::apk_dir + "/lib/armeabi-v7a/librainbowsix.so";
    if (!exists(so_path))
        fatal("Game files not found.\n\nCopy the extracted APK to ux0:data/r6sv/apk/ and the game data to "
              "ux0:data/r6sv/gamedata/ (see README).\n\nMissing: %s", so_path.c_str());
    if (!exists("ur0:data/libshacccg.suprx") && !exists("ur0:data/external/libshacccg.suprx"))
        fatal("libshacccg.suprx is not installed.\n\nExtract it with ShaRKF00D, then restart.");

    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
    vglInitExtended(0, screen::WIDTH, screen::HEIGHT, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    gl_ready = true;

    std::set_terminate(on_terminate);
    // Crash reports. An empty file named no_crash_handler in ux0:data/r6sv/ turns them off.
    if (!exists(std::string(ROOT) + "no_crash_handler")) {
        KuKernelExceptionHandlerOpt opt{sizeof(KuKernelExceptionHandlerOpt)};
        for (u32 t : HANDLED_EXCEPTIONS) {
            int r = kuKernelRegisterExceptionHandler(t, exception_handler, nullptr, &opt);
            if (r < 0) LOGW("cannot register crash handler %u (0x%08x)", t, (u32)r);
        }
    } else {
        LOGI("crash handler off (no_crash_handler file present)");
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    mem::init();
    cpu::init();
    hle::init();
    hle::init_native_direct();
    cpu::thread_attach(1, 0);

    std::vector<u8> so;
    if (!platform::read_file(so_path, so)) fatal("cannot read %s", so_path.c_str());
    const u32 reserve = 64 << 20;  // the library is ~6.4 MB plus .bss
    u32 base = mem::reserve_lib(reserve);
    Module game;
    if (!load_elf(so, base, game)) fatal("failed to load %s", so_path.c_str());
    if (game.size > reserve) fatal("game library needs %u bytes, reserved %u", game.size, reserve);
    std::vector<u8>().swap(so);
    for (auto& s : game.segments)
        if (s.flags & 1) mem::protect_exec(game.base + s.vaddr, s.memsz);
    lib_base = game.base;
    lib_size = game.size;

    java::init(game);
    LOGI("running %u constructors", (u32)game.init_funcs.size());
    run_init(game);
    if (u32 onload = game.sym("JNI_OnLoad")) LOGI("JNI_OnLoad -> %08x", cpu::call(onload, {java::vm(), 0}));

    // Game.onCreate on the Xperia Play: gamepad slider open (gamepad/touchpad
    // mode, not touch HUD). The other builds only have the touch HUD.
    if (detect_xperia_touchpad()) {
        java::call_native("Game", "nativeIsSlideEnable", {1});
        LOGI("input: Xperia Play rear touchpad");
    } else {
        hide_onscreen_stick(game);
        LOGI("input: on-screen touch controls (Triangle grenade, L aim, Circle sprint)");
    }

    // GameRenderer.onSurfaceCreated / onSurfaceChanged, at the Xperia Play's size;
    // the GL bridge scales it to the display.
    const u32 w = screen::LOGICAL_W, h = screen::LOGICAL_H;
    java::call_native("GameRenderer", "nativeInit");
    java::call_native("GLResLoader", "nativeInit");
    java::call_native("GLMediaPlayer", "nativeInit");
    java::call_native("GLUtils/Device", "nativeInit");
    java::call_native("Game", "nativeInit");
    java::call_native("GameRenderer", "nativeStart", {1, w, h, 0 /* language: English */});
    java::call_native("GameRenderer", "nativeResize", {w, h});

    TouchpadEmu pads;
    TouchscreenEmu hud;
    hud.width = w;
    hud.height = h;
    GameState state;
    state.init(game);
    u32 old_buttons = 0;
    bool touching = false, touch_blocked = false;
    int touch_x = 0, touch_y = 0;
    for (u32 frame = 0;; frame++) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        for (auto& b : BUTTONS) {
            bool now = pad.buttons & b.button, was = old_buttons & b.button;
            if (now != was) java::call_native("Game", now ? "nativeSetOnKeyDown" : "nativeSetOnKeyUp", {(u32)b.key});
        }
        if (!xperia_touchpad) {
            hud.set_active(state.playing());
            for (int i = 0; i < HUD_BUTTON_COUNT; i++) {
                auto& b = HUD_BUTTONS[i];
                bool now = pad.buttons & b.button, was = old_buttons & b.button;
                if (now != was) hud.hud(i, b.x, b.y, now);
            }
        }
        old_buttons = pad.buttons;

        // sticks -> rear touchpad on the Xperia build, on-screen stick and a
        // dragging finger on the others (left moves, right looks; y up)
        float mx = stick_axis(pad.lx), my = -stick_axis(pad.ly);
        float lx = stick_axis(pad.rx) * LOOK_SPEED, ly = -stick_axis(pad.ry) * LOOK_SPEED;
        if (xperia_touchpad) {
            pads.stick(0, mx, my);
            pads.look(lx, ly, false);
        } else {
            hud.stick(0, mx, my);
            hud.look(lx, ly, false);
        }

        // front touch screen -> the phone's touch screen (one finger)
        SceTouchData touch{};
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum > 0) {
            // reports are in 1920x1088 panel units
            int x = touch.report[0].x * screen::LOGICAL_W / 1920, y = touch.report[0].y * screen::LOGICAL_H / 1088;
            // a touch that starts on the hidden stick is ignored until lifted
            if (!touching && !touch_blocked && !xperia_touchpad && hud.blocks_touch(x, y)) touch_blocked = true;
            if (!touch_blocked) {
                if (!touching || x != touch_x || y != touch_y) {
                    screen_touch_flags();
                    java::call_native("Game", touching ? "nativeTouchMoved" : "nativeTouchPressed", {(u32)x, (u32)y, 0});
                }
                touching = true;
                touch_x = x;
                touch_y = y;
            }
        } else {
            touch_blocked = false;
            if (touching) {
                touching = false;
                screen_touch_flags();
                java::call_native("Game", "nativeTouchReleased", {(u32)touch_x, (u32)touch_y, 0});
            }
        }

        java::call_native("GameRenderer", "nativeRender");
        static int gl_errors = 0;
        for (GLenum e; gl_errors < 30 && (e = glGetError()) != GL_NO_ERROR; gl_errors++)
            LOGW("GL error %04x during frame %u", e, frame);
        platform::swap_buffers();  // GLSurfaceView swaps after every onDrawFrame
        if (frame % 600 == 0) {
            struct mallinfo mi = mallinfo();
            LOGI("frame %u, heap %u MB in use (%u MB reserved of %u)", frame, (u32)mi.uordblks >> 20, (u32)mi.arena >> 20,
                 (u32)_newlib_heap_size_user >> 20);
        }
    }
}
