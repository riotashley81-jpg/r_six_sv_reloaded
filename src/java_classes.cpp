// Host reimplementations of the game's Java classes and the Android APIs
// its native code calls through JNI.
#include <filesystem>

#include "hle.h"
#include "jni.h"
#include "os.h"
#include "platform.h"

namespace java {
namespace {

const std::string P = "com/gameloft/android/ANMP/GloftR6HP/";
using Args = const std::vector<Value>&;

// GLResLoader.getResourceBytes mirrors the Java code: "./x/y/name.ext" is an
// Android drawable "res_name"; anything else is read from the APK assets.
bool load_resource(const std::string& name, std::vector<u8>& out) {
    std::string n = name;
    if (n.rfind(".//", 0) == 0) n = n.substr(3);
    else if (n.rfind("./", 0) == 0) n = n.substr(2);
    else {
        if (platform::read_file(platform::apk_dir + "/assets/" + n, out)) return true;
        return platform::read_file(hle::host_path(n.c_str()), out);
    }
    std::string base = n.substr(n.rfind('/') + 1);
    for (auto& ch : base) ch = (char)tolower(ch);
    base = "res_" + base.substr(0, base.find('.'));
    std::error_code ec;
    for (auto& dir : std::filesystem::directory_iterator(platform::apk_dir + "/res", ec)) {
        for (auto& f : std::filesystem::directory_iterator(dir.path(), ec))
            if (f.path().stem().string() == base) return platform::read_file(f.path().string(), out);
    }
    return false;
}

void res_loader() {
    std::string C = P + "GLResLoader";
    def(C, "getResourceBytes", "(Ljava/lang/String;II)[B", [](u32, Args a) -> Value {
        std::string name = string_of((u32)a[0]);
        u32 off = (u32)a[1], len = (u32)a[2];
        std::vector<u8> data;
        if (!load_resource(name, data) || off > data.size()) {
            LOGW("getResourceBytes(%s) not found", name.c_str());
            return 0;
        }
        len = std::min<u32>(len, (u32)data.size() - off);
        return new_byte_array(data.data() + off, len);
    });
    def(C, "getResourceLength", "(Ljava/lang/String;)I", [](u32, Args a) -> Value {
        std::vector<u8> data;
        std::string name = string_of((u32)a[0]);
        if (!load_resource(name, data)) {
            LOGW("getResourceLength(%s) not found", name.c_str());
            return (Value)(u32)-1;
        }
        return (u32)data.size();
    });
    def(C, "getResourceFull", "(Ljava/lang/String;)[B", [](u32, Args a) -> Value {
        std::vector<u8> data;
        std::string name = string_of((u32)a[0]);
        if (!load_resource(name, data)) {
            LOGW("getResourceFull(%s) not found", name.c_str());
            return 0;
        }
        return new_byte_array(data.data(), (u32)data.size());
    });
    // integer ids refer to R.raw resources; logged until we see them used
    def(C, "getResourceFull", "(I)[B", [](u32, Args a) -> Value {
        LOGW("getResourceFull(id %d)", (int)a[0]);
        return 0;
    });
    def(C, "getSoundRaw", "(I)[B", [](u32, Args a) -> Value {
        LOGW("getSoundRaw(%d)", (int)a[0]);
        return 0;
    });
    def(C, "getResourceLengthSoundRaw", "(I)I", [](u32, Args a) -> Value {
        LOGW("getResourceLengthSoundRaw(%d)", (int)a[0]);
        return 0;
    });
}

void media_player() {
    // Sound effects and music go through Android SoundPool/MediaPlayer on the
    // Java side. Until the mixer exists these just log.
    std::string C = P + "GLMediaPlayer";
    auto quiet = [](const char* what) {
        return [what](u32, Args a) -> Value {
            static int n = 0;
            if (n++ < 5) LOGI("GLMediaPlayer.%s(%d)", what, a.empty() ? 0 : (int)a[0]);
            return 0;
        };
    };
    def(C, "play", "(IFI)I", quiet("play"));
    def(C, "play", "(IFIF)I", quiet("play"));
    def(C, "stop", "(I)V", quiet("stop"));
    def(C, "stopAll", "()V", quiet("stopAll"));
    def(C, "stopAllMusic", "()V", quiet("stopAllMusic"));
    def(C, "stopEmitter", "(II)V", quiet("stopEmitter"));
    def(C, "setEmitterParams", "(IIFF)V", quiet("setEmitterParams"));
    def(C, "setEmitterPitch", "(IIF)V", quiet("setEmitterPitch"));
    def(C, "setEmitterVolume", "(IIF)V", quiet("setEmitterVolume"));
    def(C, "isEmitterPlaying", "(II)Z", quiet("isEmitterPlaying"));
    def(C, "isPlaying", "(I)Z", quiet("isPlaying"));
    def(C, "loadSoundGroup", "(IZ)V", quiet("loadSoundGroup"));
    def(C, "pauseAll", "()V", quiet("pauseAll"));
    def(C, "resumeAll", "(F)V", quiet("resumeAll"));
    def(C, "setVolume", "(FI)V", quiet("setVolume"));
    def(C, "getVolume", "(I)F", [](u32, Args) -> Value { return F(1.0f); });
    def(C, "getMasterVolume", "()F", [](u32, Args) -> Value { return F(1.0f); });
}

void renderer() {
    std::string C = P + "GameRenderer";
    def(C, "swapEGLBuffers", "()V", [](u32, Args) -> Value {
        platform::swap_buffers();
        return 0;
    });
    def(C, "getKeyboardText", "()[B", [](u32, Args) -> Value { return new_byte_array("", 0); });
    def(C, "isKeyboardRequired", "()I", [](u32, Args) -> Value { return 0; });
    def(C, "setKeyboard", "(ILjava/lang/String;I)V", [](u32, Args a) -> Value {
        LOGI("setKeyboard(%d, \"%s\", %d)", (int)a[0], string_of((u32)a[1]).c_str(), (int)a[2]);
        return 0;
    });
}

void game_and_device() {
    std::string G = P + "Game";
    def(G, "GetManufacturer", "()Ljava/lang/String;", [](u32, Args) -> Value { return new_string("Sony Ericsson"); });
    // Application::isXperiaPlayPhone() looks for "r800" (case-sensitive) in the model;
    // it gates the gamepad button handling
    def(G, "GetModel", "()Ljava/lang/String;", [](u32, Args) -> Value { return new_string("r800i"); });
    def(G, "GetPhoneLanguage", "()I", [](u32, Args) -> Value { return 0; });  // English
    // rear touchpad state (see platform::JavaTouchpad)
    def(G, "isMoveMC", "()I", [](u32, Args) -> Value {
        auto& t = platform::touchpad;
        return t.M != -1 && t.N != -1 && t.M < 380 ? 1 : (u32)-1;
    });
    def(G, "getMoveCamera_X", "()I", [](u32, Args) -> Value { return (u32)platform::touchpad.O; });
    def(G, "getMoveCamera_Y", "()I", [](u32, Args) -> Value { return (u32)platform::touchpad.P; });
    def(G, "getMoveCamera_Flexible_X", "()I", [](u32, Args) -> Value { return (u32)platform::touchpad.Q; });
    def(G, "getMoveCamera_Flexible_Y", "()I", [](u32, Args) -> Value { return (u32)platform::touchpad.R; });
    def(G, "isSlideEnable", "()I", [](u32, Args) -> Value { return 1; });  // gamepad slider open
    def(G, "isSlideChange", "()I", [](u32, Args) -> Value { return 0; });
    def(G, "getGPUInfo", "()I", [](u32, Args) -> Value {
        const char* e = getenv("R6SV_GPU");
        return e ? (u32)atoi(e) : 0;
    });
    def(G, "IsWifiEnabled", "()I", [](u32, Args) -> Value { return 0; });
    def(G, "db", "()[B", [](u32, Args) -> Value { return new_byte_array("", 0); });
    for (const char* m : {"ShowSpinner"})
        def(G, m, "(Ljava/lang/String;)V", [](u32, Args) -> Value { return 0; });
    for (const char* m : {"DismissSpinner", "sendAppToBackground", "launchGetGames"})
        def(G, m, "()V", [](u32, Args) -> Value { return 0; });
    for (const char* m : {"SetDeviceMute", "notifyTrophy", "launchIGP", "SetReorientation"})
        def(G, m, "(I)V", [](u32, Args) -> Value { return 0; });
    def(G, "Exit", "()V", [](u32, Args) -> Value {
        LOGI("game requested exit");
        os::exit_process(0);
    });
    def(G, "launchGLLive", "(I)V", [](u32, Args a) -> Value {
        LOGI("launchGLLive(%d) ignored", (int)a[0]);
        return 0;
    });
    def(G, "openBrowser", "(Ljava/lang/String;)V", [](u32, Args a) -> Value {
        LOGI("openBrowser(%s) ignored", string_of((u32)a[0]).c_str());
        return 0;
    });
    std::string D = P + "GLUtils/Device";
    def(D, "getDeviceId", "()Ljava/lang/String;", [](u32, Args) -> Value { return new_string("000000000000000"); });
    def(D, "IsWifiEnable", "()Z", [](u32, Args) -> Value { return 0; });
}

// android.media.AudioTrack: the vox engine mixes PCM and streams it here.
void audio_track() {
    std::string C = "android/media/AudioTrack";
    def(C, "getMinBufferSize", "(III)I", [](u32, Args a) -> Value {
        LOGI("AudioTrack.getMinBufferSize(%d, %d, %d)", (int)a[0], (int)a[1], (int)a[2]);
        return 8192;
    });
    def(C, "<init>", "(IIIIII)V", [](u32, Args a) -> Value {
        // (streamType, sampleRate, channelConfig, audioFormat, bufferSize, mode)
        u32 rate = (u32)a[1], cfg = (u32)a[2];
        u32 channels = (cfg == 3 || cfg == 12) ? 2 : 1;  // CHANNEL_CONFIGURATION_STEREO / CHANNEL_OUT_STEREO
        LOGI("AudioTrack(rate=%u, channels=%u, format=%d, buffer=%d)", rate, channels, (int)a[3], (int)a[4]);
        platform::audio_open(rate, channels);
        return 0;
    });
    def(C, "write", "([BII)I", [](u32, Args a) -> Value {
        auto arr = get_as<Array>((u32)a[0]);
        u32 off = (u32)a[1], len = (u32)a[2];
        if (!arr || off + len > arr->len) return (Value)(u32)-1;
        platform::audio_write(mem::H(arr->data + off), len);
        return len;
    });
    for (const char* m : {"play", "stop", "pause", "flush", "release"})
        def(C, m, "()V", [](u32, Args) -> Value { return 0; });
    def(C, "setStereoVolume", "(FF)I", [](u32, Args) -> Value { return 0; });
    def(C, "getPlayState", "()I", [](u32, Args) -> Value { return 3; });  // PLAYSTATE_PLAYING
}

}  // namespace

void register_classes() {
    res_loader();
    media_player();
    renderer();
    game_and_device();
    audio_track();
}

}  // namespace java
