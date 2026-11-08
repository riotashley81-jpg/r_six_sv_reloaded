// Services the Java layer needs from the host frontend (window, audio, APK).
#pragma once
#include <string>
#include <vector>

#include "common.h"

namespace platform {

void swap_buffers();
void audio_open(u32 rate, u32 channels);
void audio_write(const void* pcm, u32 bytes);  // 16-bit PCM, blocks like AudioTrack.write

// Game's static touchpad fields, as Game.onTouchEvent maintains them from raw
// rear-touchpad coordinates; the native code polls them through
// Game.isMoveMC / getMoveCamera_*.
struct JavaTouchpad {
    int M = -1, N = -1;  // left (move) finger, -1 when lifted
    int C = 0, D = 0;    // right finger touch-down point
    int O = 0, P = 0;    // right finger offset from the pad centre (y up)
    int Q = 0, R = 0;    // right finger offset from its touch-down point (y up)
};
extern JavaTouchpad touchpad;

// Directory holding the extracted APK (res/, assets/).
extern std::string apk_dir;
bool read_file(const std::string& path, std::vector<u8>& out);

}  // namespace platform
