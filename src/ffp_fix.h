// Fix-ups for the engine's OpenGL ES 1.1 (fixed-function) renderer, shared by
// the PC and Vita GL bridges. The bridges report the state calls the game makes
// and ask before each draw whether a fix-up applies.
//
// Reflective weapons: texture unit 0 holds the diffuse, unit 1 a sphere map,
// and unit 1 is set to INTERPOLATE(previous, sphere, 1 - previous.alpha), so the
// diffuse texture's alpha is the reflectivity. Most weapon textures have no
// alpha (ATC RGB), which makes them 100% mirror. For those, the draw uses a
// fixed reflectivity instead.
#pragma once

#include "common.h"

namespace ffp_fix {

// Host GL entry points the fix-ups call (set once by the bridge).
struct Host {
    void (*active_texture)(u32 unit);
    void (*tex_envi)(u32 target, u32 pname, s32 value);
    void (*tex_envfv)(u32 target, u32 pname, const float* v);
};
void set_host(const Host& h);

// State calls, as the game makes them (call alongside forwarding them).
void active_texture(u32 unit);
void bind_texture(u32 target, u32 tex);
void enable(u32 cap, bool on);
void tex_envi(u32 target, u32 pname, s32 value);
void tex_envfv(u32 target, u32 pname, const float* v);
// After a texture image upload to level 0 of the bound texture.
void tex_image(u32 level, bool has_alpha);
bool format_has_alpha(u32 internal_format);  // GL internal or compressed format

// Wrap each draw call: before_draw() returns true when it changed state,
// and then after_draw() must be called after the draw.
bool before_draw();
void after_draw();

}  // namespace ffp_fix
