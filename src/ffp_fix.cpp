#include "ffp_fix.h"

#include <unordered_map>

namespace ffp_fix {
namespace {

enum : u32 {
    TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE_ENV = 0x2300, TEXTURE_ENV_MODE = 0x2200,
    TEXTURE_ENV_COLOR = 0x2201, COMBINE = 0x8570, COMBINE_RGB = 0x8571, INTERPOLATE = 0x8575,
    SRC2_RGB = 0x8582, OPERAND2_RGB = 0x8592, CONSTANT = 0x8576, PREVIOUS = 0x8578,
    SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303,
};

// How much of the sphere map a weapon without a reflectivity mask shows.
constexpr float REFLECTIVITY = 0.3f;

struct Unit {
    u32 tex = 0;
    bool enabled = false;
    u32 mode = 0x2100;  // MODULATE
    u32 combine_rgb = 0x2100, src2_rgb = CONSTANT, operand2_rgb = SRC_ALPHA;
    float env_color[4] = {0, 0, 0, 0};
};

Host host{};
Unit units[8];
u32 active = 0;
std::unordered_map<u32, bool> alpha;  // texture id -> level 0 has an alpha channel
bool patched = false;

}  // namespace

void set_host(const Host& h) { host = h; }

void active_texture(u32 unit) {
    if (unit >= TEXTURE0 && unit < TEXTURE0 + 8) active = unit - TEXTURE0;
}

void bind_texture(u32 target, u32 tex) {
    if (target == TEXTURE_2D) units[active].tex = tex;
}

void enable(u32 cap, bool on) {
    if (cap == TEXTURE_2D) units[active].enabled = on;
}

void tex_envi(u32 target, u32 pname, s32 value) {
    if (target != TEXTURE_ENV) return;
    Unit& u = units[active];
    switch (pname) {
    case TEXTURE_ENV_MODE: u.mode = (u32)value; break;
    case COMBINE_RGB: u.combine_rgb = (u32)value; break;
    case SRC2_RGB: u.src2_rgb = (u32)value; break;
    case OPERAND2_RGB: u.operand2_rgb = (u32)value; break;
    }
}

void tex_envfv(u32 target, u32 pname, const float* v) {
    if (target == TEXTURE_ENV && pname == TEXTURE_ENV_COLOR)
        for (int i = 0; i < 4; i++) units[active].env_color[i] = v[i];
}

void tex_image(u32 level, bool has_alpha) {
    if (level == 0) alpha[units[active].tex] = has_alpha;
}

bool format_has_alpha(u32 f) {
    switch (f) {
    case 0x1907:                     // RGB
    case 0x1909:                     // LUMINANCE
    case 0x8D64:                     // ETC1
    case 0x83F0:                     // DXT1 RGB
    case 0x8C92:                     // ATC RGB
    case 0x8C00: case 0x8C01:        // PVRTC RGB
        return false;
    default:
        return true;
    }
}

bool before_draw() {
    const Unit& base = units[0];
    const Unit& refl = units[1];
    if (!host.tex_envi || !base.enabled || !refl.enabled || refl.mode != COMBINE || refl.combine_rgb != INTERPOLATE ||
        refl.src2_rgb != PREVIOUS || refl.operand2_rgb != ONE_MINUS_SRC_ALPHA)
        return false;
    auto it = alpha.find(base.tex);
    if (it == alpha.end() || it->second) return false;  // a real reflectivity mask: leave it
    // INTERPOLATE weight = 1 - constant.alpha, so the sphere map gets REFLECTIVITY
    host.active_texture(TEXTURE0 + 1);
    const float c[4] = {refl.env_color[0], refl.env_color[1], refl.env_color[2], REFLECTIVITY};
    host.tex_envfv(TEXTURE_ENV, TEXTURE_ENV_COLOR, c);
    host.tex_envi(TEXTURE_ENV, SRC2_RGB, CONSTANT);
    host.active_texture(TEXTURE0 + active);
    patched = true;
    return true;
}

void after_draw() {
    if (!patched) return;
    patched = false;
    host.active_texture(TEXTURE0 + 1);
    host.tex_envi(TEXTURE_ENV, SRC2_RGB, (s32)units[1].src2_rgb);
    host.tex_envfv(TEXTURE_ENV, TEXTURE_ENV_COLOR, units[1].env_color);
    host.active_texture(TEXTURE0 + active);
}

}  // namespace ffp_fix
