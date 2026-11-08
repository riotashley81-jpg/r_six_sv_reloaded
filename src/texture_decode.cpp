#include "texture_decode.h"

namespace tex {
namespace {

struct RGB { int r, g, b; };

int expand5(int v) { return (v << 3) | (v >> 2); }
int expand6(int v) { return (v << 2) | (v >> 4); }
int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// ATC colour block: color0 is a mode bit + RGB555, color1 is RGB565, then
// 2-bit indices for the 16 texels (row-major, LSB first).
void atc_color(const u8* b, u8 out[16][4]) {
    u16 c0 = (u16)(b[0] | (b[1] << 8));
    u16 c1 = (u16)(b[2] | (b[3] << 8));
    u32 idx = (u32)(b[4] | (b[5] << 8) | (b[6] << 16) | ((u32)b[7] << 24));
    RGB a{expand5((c0 >> 10) & 31), expand5((c0 >> 5) & 31), expand5(c0 & 31)};
    RGB z{expand5((c1 >> 11) & 31), expand6((c1 >> 5) & 63), expand5(c1 & 31)};
    RGB pal[4];
    if (c0 & 0x8000) {
        pal[0] = {0, 0, 0};
        pal[1] = {clamp8(a.r - z.r / 4), clamp8(a.g - z.g / 4), clamp8(a.b - z.b / 4)};
        pal[2] = a;
        pal[3] = z;
    } else {
        pal[0] = a;
        pal[1] = {(5 * a.r + 3 * z.r) / 8, (5 * a.g + 3 * z.g) / 8, (5 * a.b + 3 * z.b) / 8};
        pal[2] = {(3 * a.r + 5 * z.r) / 8, (3 * a.g + 5 * z.g) / 8, (3 * a.b + 5 * z.b) / 8};
        pal[3] = z;
    }
    for (int i = 0; i < 16; i++) {
        const RGB& c = pal[(idx >> (2 * i)) & 3];
        out[i][0] = (u8)c.r;
        out[i][1] = (u8)c.g;
        out[i][2] = (u8)c.b;
        out[i][3] = 255;
    }
}

// 4-bit explicit alpha (as in DXT3)
void alpha_explicit(const u8* b, u8 out[16][4]) {
    for (int i = 0; i < 16; i++) {
        int v = (b[i / 2] >> ((i & 1) * 4)) & 15;
        out[i][3] = (u8)(v * 17);
    }
}

// interpolated alpha (as in DXT5)
void alpha_interpolated(const u8* b, u8 out[16][4]) {
    int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i][3] = (u8)pal[(bits >> (3 * i)) & 7];
}

}  // namespace

bool is_atc(u32 f) { return f == ATC_RGB || f == ATC_RGBA_EXPLICIT || f == ATC_RGBA_INTERPOLATED; }

u32 atc_size(u32 f, u32 w, u32 h) {
    u32 blocks = ((w + 3) / 4) * ((h + 3) / 4);
    return f == ATC_RGB ? blocks * 8 : is_atc(f) ? blocks * 16 : 0;
}

bool decode_atc(u32 f, const u8* src, u32 w, u32 h, std::vector<u8>& rgba) {
    if (!is_atc(f)) return false;
    rgba.assign((size_t)w * h * 4, 0);
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    u32 block_size = f == ATC_RGB ? 8 : 16;
    u8 texels[16][4];
    for (u32 by = 0; by < bh; by++) {
        for (u32 bx = 0; bx < bw; bx++, src += block_size) {
            if (f == ATC_RGB) {
                atc_color(src, texels);
            } else {
                atc_color(src + 8, texels);
                if (f == ATC_RGBA_EXPLICIT) alpha_explicit(src, texels);
                else alpha_interpolated(src, texels);
            }
            for (u32 y = 0; y < 4; y++) {
                u32 py = by * 4 + y;
                if (py >= h) break;
                for (u32 x = 0; x < 4; x++) {
                    u32 px = bx * 4 + x;
                    if (px >= w) break;
                    memcpy(&rgba[((size_t)py * w + px) * 4], texels[y * 4 + x], 4);
                }
            }
        }
    }
    return true;
}

}  // namespace tex
