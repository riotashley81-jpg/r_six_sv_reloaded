// Software decoders for mobile GPU texture formats the host can't sample.
#pragma once
#include <vector>

#include "common.h"

namespace tex {

enum : u32 {
    ATC_RGB = 0x8C92,                     // GL_ATC_RGB_AMD
    ATC_RGBA_EXPLICIT = 0x8C93,           // GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
    ATC_RGBA_INTERPOLATED = 0x87EE,       // GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD
};

bool is_atc(u32 format);
// Compressed size the format needs for a w x h image (0 if unknown).
u32 atc_size(u32 format, u32 w, u32 h);
// Decode to tightly packed RGBA8; returns false for unknown formats.
bool decode_atc(u32 format, const u8* src, u32 w, u32 h, std::vector<u8>& rgba);

// The LG Optimus 2X data stores its textures as DXT5, which only the LG build
// loads; the others take ATC. dxt5_to_atc turns one 16-byte DXT5 block into
// an ATC interpolated-alpha block in place (the alpha half is the same format;
// colour endpoint 0 loses its lowest green bit). atc_to_dxt5 goes back, so the
// GPU gets DXT5 again; it fails if a block uses ATC's second colour mode, which
// DXT can't express (then the caller decodes on the CPU as before).
void dxt5_to_atc(u8* block);
bool atc_to_dxt5(const u8* src, u32 size, std::vector<u8>& out);

}  // namespace tex
