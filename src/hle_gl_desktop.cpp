// GLES 1.1/2.0 -> desktop OpenGL (compatibility profile) bridge.
// Desktop compat GL accepts both the fixed-function and the shader API, so
// most calls pass straight through; the rest need pointer translation
// (guest client arrays vs. VBO offsets), ES-only entry points, or GLSL ES
// shader rewriting.
#include <SDL.h>
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <regex>
#include <unordered_map>

#include "ffp_fix.h"
#include "hle.h"
#include "texture_decode.h"

namespace hle {
namespace {

#ifndef APIENTRY
#define APIENTRY __stdcall
#endif

// Fn<Id, R, A...> holds one host GL entry point and a host-callable wrapper.
template <int Id, class R, class... A> struct Fn {
    static inline R(APIENTRY* p)(A...) = nullptr;
    static R call(A... a) { return p(a...); }
};

void* proc(const char* name) {
    void* p = SDL_GL_GetProcAddress(name);
    if (!p) LOGW("GL: host has no %s", name);
    return p;
}

// PASS(guest name, host name, return, args...) registers a straight pass-through.
#define PASS(gname, hname, R, ...)                                     \
    {                                                                  \
        using F = Fn<__LINE__, R, ##__VA_ARGS__>;                      \
        F::p = (decltype(F::p))proc(hname);                            \
        if (F::p) reg(gname, W<&F::call>);                             \
    }
// HOST(var, name, R, args...) loads an entry point for custom wrappers.
#define HOST(var, name, R, ...) var = (R(APIENTRY*)(__VA_ARGS__))proc(name)

enum : u32 {
    ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, BUFFER_SIZE = 0x8764, FIXED = 0x140C,
    VERTEX_SHADER = 0x8B31, NUM_COMPRESSED = 0x86A2, COMPRESSED_FORMATS = 0x86A3,
};

u32 bound_array = 0, bound_element = 0;
u32 str_vendor, str_renderer, str_version, str_glsl, str_ext;

void(APIENTRY* g_BindBuffer)(u32, u32);
void(APIENTRY* g_BufferData)(u32, ptrdiff_t, const void*, u32);
void(APIENTRY* g_BufferSubData)(u32, ptrdiff_t, ptrdiff_t, const void*);
void(APIENTRY* g_GetBufferParameteriv)(u32, u32, s32*);
void(APIENTRY* g_VertexAttribPointer)(u32, s32, u32, u8, s32, const void*);
void(APIENTRY* g_VertexPointer)(s32, u32, s32, const void*);
void(APIENTRY* g_ColorPointer)(s32, u32, s32, const void*);
void(APIENTRY* g_TexCoordPointer)(s32, u32, s32, const void*);
void(APIENTRY* g_NormalPointer)(u32, s32, const void*);
void(APIENTRY* g_DrawElements)(u32, s32, u32, const void*);
void(APIENTRY* g_ClearDepth)(double);
void(APIENTRY* g_DepthRange)(double, double);
void(APIENTRY* g_ClipPlane)(u32, const double*);
void(APIENTRY* g_GetIntegerv)(u32, s32*);
void(APIENTRY* g_ShaderSource)(u32, s32, const char* const*, const s32*);
u32(APIENTRY* g_CreateShader)(u32);
void(APIENTRY* g_CompressedTexImage2D)(u32, s32, u32, s32, s32, s32, s32, const void*);
void(APIENTRY* g_CompressedTexSubImage2D)(u32, s32, s32, s32, s32, s32, u32, s32, const void*);
void(APIENTRY* g_TexImage2D)(u32, s32, s32, s32, s32, s32, u32, u32, const void*);
void(APIENTRY* g_TexSubImage2D)(u32, s32, s32, s32, s32, s32, u32, u32, const void*);

// --- buffers & pointers ---------------------------------------------------------
void h_BindBuffer(u32 target, u32 buf) {
    if (target == ARRAY_BUFFER) bound_array = buf;
    else if (target == ELEMENT_ARRAY_BUFFER) bound_element = buf;
    g_BindBuffer(target, buf);
}
void h_BufferData(u32 t, s32 size, const void* data, u32 usage) { g_BufferData(t, size, data, usage); }
void h_BufferSubData(u32 t, s32 off, s32 size, const void* data) { g_BufferSubData(t, off, size, data); }

// R6SV_GLTRACE: log each distinct vertex format / draw call shape once, and
// save shader sources to shaders/<n>.glsl
bool gl_trace = getenv("R6SV_GLTRACE") != nullptr;
void trace_once(const char* fmt, ...) {
    static std::unordered_map<std::string, int> seen;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (seen[buf]++ == 0) LOGI("GLTRACE %s", buf);
}

// with a VBO bound the "pointer" is a byte offset into it
const void* array_ptr(u32 p) { return bound_array ? (const void*)(uintptr_t)p : mem::H(p); }
void warn_fixed(u32 type) {
    static bool once;
    if (type == FIXED && !once) {
        once = true;
        LOGW("GL: GL_FIXED vertex data is not supported on desktop GL yet");
    }
}
void h_VertexAttribPointer(u32 i, s32 size, u32 type, u8 norm, s32 stride, u32 p) {
    warn_fixed(type);
    if (gl_trace) trace_once("VertexAttribPointer idx=%u size=%d type=%04x norm=%u stride=%d vbo=%d", i, size, type, norm, stride, bound_array != 0);
    g_VertexAttribPointer(i, size, type, norm, stride, array_ptr(p));
}
void h_VertexPointer(s32 size, u32 type, s32 stride, u32 p) {
    if (gl_trace) trace_once("VertexPointer size=%d type=%04x stride=%d vbo=%d", size, type, stride, bound_array != 0);
    warn_fixed(type); g_VertexPointer(size, type, stride, array_ptr(p)); }
void h_ColorPointer(s32 size, u32 type, s32 stride, u32 p) { g_ColorPointer(size, type, stride, array_ptr(p)); }
void h_TexCoordPointer(s32 size, u32 type, s32 stride, u32 p) {
    if (gl_trace) trace_once("TexCoordPointer size=%d type=%04x stride=%d vbo=%d", size, type, stride, bound_array != 0);
    warn_fixed(type); g_TexCoordPointer(size, type, stride, array_ptr(p)); }
void h_NormalPointer(u32 type, s32 stride, u32 p) { warn_fixed(type); g_NormalPointer(type, stride, array_ptr(p)); }
// --- ES 1.1 state the fixed-function fix-ups track (see ffp_fix.h) -------------------
void(APIENTRY* g_ActiveTexture)(u32);
void(APIENTRY* g_BindTexture)(u32, u32);
void(APIENTRY* g_Enable)(u32);
void(APIENTRY* g_Disable)(u32);
void(APIENTRY* g_TexEnvi)(u32, u32, s32);
void(APIENTRY* g_TexEnvf)(u32, u32, float);
void(APIENTRY* g_TexEnvfv)(u32, u32, const float*);
void(APIENTRY* g_DrawArrays)(u32, s32, s32);
void h_ActiveTexture(u32 u) { ffp_fix::active_texture(u); g_ActiveTexture(u); }
void h_BindTexture(u32 t, u32 tex) { ffp_fix::bind_texture(t, tex); g_BindTexture(t, tex); }
void h_Enable(u32 cap) { ffp_fix::enable(cap, true); g_Enable(cap); }
void h_Disable(u32 cap) { ffp_fix::enable(cap, false); g_Disable(cap); }
void h_TexEnvi(u32 t, u32 p, s32 v) { ffp_fix::tex_envi(t, p, v); g_TexEnvi(t, p, v); }
void h_TexEnvf(u32 t, u32 p, float v) { ffp_fix::tex_envi(t, p, (s32)v); g_TexEnvf(t, p, v); }
void h_TexEnvfv(u32 t, u32 p, const float* v) { ffp_fix::tex_envfv(t, p, v); g_TexEnvfv(t, p, v); }
void h_DrawArrays(u32 mode, s32 first, s32 count) {
    bool fixed = ffp_fix::before_draw();
    g_DrawArrays(mode, first, count);
    if (fixed) ffp_fix::after_draw();
}

u32 bound_tex();
void h_DrawElements(u32 mode, s32 count, u32 type, u32 p) {
    if (gl_trace) trace_once("DrawElements mode=%u type=%04x ibo=%d", mode, type, bound_element != 0);
    // R6SV_HIDETEX=id[,id...]: skip draws that use these textures (to find what is what)
    static std::vector<u32> hide = [] {
        std::vector<u32> v;
        if (const char* s = getenv("R6SV_HIDETEX"))
            for (const char* p = s; *p;) { v.push_back((u32)strtoul(p, (char**)&p, 16)); if (*p == ',') p++; }
        return v;
    }();
    if (!hide.empty() && std::find(hide.begin(), hide.end(), bound_tex()) != hide.end()) return;
    if (cpu::trace_gl_calls) {
        // the host's view of the texture state at this draw
        static void(APIENTRY * getf)(u32, float*) = (void(APIENTRY*)(u32, float*))proc("glGetFloatv");
        float m[16];
        s32 unit = 0, mm = 0;
        getf(0x0BA8, m);  // GL_TEXTURE_MATRIX
        g_GetIntegerv(0x84E0, &unit);  // GL_ACTIVE_TEXTURE
        g_GetIntegerv(0x0BA0, &mm);    // GL_MATRIX_MODE
        LOGI("GL   host: active %x, matrix mode %x, texture matrix scale %g %g offset %g %g", unit, mm, m[0], m[5], m[12], m[13]);
        // texture environment of units 0 and 1
        static void(APIENTRY * active)(u32) = (void(APIENTRY*)(u32))proc("glActiveTexture");
        static void(APIENTRY * getenv_i)(u32, u32, s32*) = (void(APIENTRY*)(u32, u32, s32*))proc("glGetTexEnviv");
        static void(APIENTRY * getenv_f)(u32, u32, float*) = (void(APIENTRY*)(u32, u32, float*))proc("glGetTexEnvfv");
        static u8(APIENTRY * enabled)(u32) = (u8(APIENTRY*)(u32))proc("glIsEnabled");
        for (u32 u = 0; u < 2; u++) {
            active(0x84C0 + u);
            s32 e[16];
            const u32 pn[] = {0x2200, 0x8571, 0x8580, 0x8590, 0x8581, 0x8591, 0x8582, 0x8592,
                              0x8572, 0x8588, 0x8598, 0x8589, 0x8599};
            for (int i = 0; i < 13; i++) getenv_i(0x2300, pn[i], &e[i]);
            float col[4], scale[2];
            getenv_f(0x2300, 0x2201, col);
            getenv_f(0x2300, 0x8573, &scale[0]);
            getenv_f(0x2300, 0x0D1C, &scale[1]);
            s32 tex = 0;
            g_GetIntegerv(0x8069, &tex);
            LOGI("GL   unit%u tex2d=%d tex=%x mode=%x | rgb %x(%x:%x, %x:%x, %x:%x) | a %x(%x:%x, %x:%x) | color %.2f %.2f %.2f %.2f scale %g %g",
                 u, enabled(0x0DE1), tex, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12],
                 col[0], col[1], col[2], col[3], scale[0], scale[1]);
        }
        active(unit);
    }
    bool fixed = ffp_fix::before_draw();
    g_DrawElements(mode, count, type, bound_element ? (const void*)(uintptr_t)p : mem::H(p));
    if (fixed) ffp_fix::after_draw();
}

// glMapBufferOES: the guest can only see guest memory, so hand out a shadow
// copy and upload it on unmap (write-only mapping is all ES 2.0 allows).
struct Mapping { u32 guest = 0, size = 0; };
std::unordered_map<u32, Mapping> mappings;  // by target
u32 h_MapBuffer(u32 target, u32) {
    s32 size = 0;
    g_GetBufferParameteriv(target, BUFFER_SIZE, &size);
    if (size <= 0) return 0;
    Mapping& m = mappings[target];
    m.guest = mem::alloc((u32)size);
    m.size = (u32)size;
    return m.guest;
}
u32 h_UnmapBuffer(u32 target) {
    auto it = mappings.find(target);
    if (it == mappings.end()) return 0;
    g_BufferSubData(target, 0, it->second.size, mem::H(it->second.guest));
    mem::free(it->second.guest);
    mappings.erase(it);
    return 1;
}

// --- ES-only entry points -------------------------------------------------------
void h_ClearDepthf(float d) { g_ClearDepth(d); }
void h_DepthRangef(float n, float f) { g_DepthRange(n, f); }
void h_ClipPlanef(u32 plane, const float* eq) {
    double d[4] = {eq[0], eq[1], eq[2], eq[3]};
    g_ClipPlane(plane, d);
}

// --- queries ----------------------------------------------------------------------
u32 h_GetString(u32 name) {
    switch (name) {
    case 0x1F00: return str_vendor;
    case 0x1F01: return str_renderer;
    case 0x1F02: return str_version;
    case 0x1F03: return str_ext;
    case 0x8B8C: return str_glsl;
    }
    return 0;
}
// DXT goes to the GPU as-is; ATC (Adreno builds) is decoded on the CPU.
const u32 COMPRESSED[] = {0x83F0, 0x83F1, 0x83F2, 0x83F3, tex::ATC_RGB, tex::ATC_RGBA_EXPLICIT, tex::ATC_RGBA_INTERPOLATED};
void h_GetIntegerv(u32 pname, s32* p) {
    if (pname == NUM_COMPRESSED) { *p = (s32)std::size(COMPRESSED); return; }
    if (pname == COMPRESSED_FORMATS) { for (u32 f : COMPRESSED) *p++ = (s32)f; return; }
    g_GetIntegerv(pname, p);
}
void unsupported_format(u32 fmt, s32 w, s32 h) {
    static int n;
    if (n++ < 10) LOGW("GL: unsupported compressed texture format %04x (%dx%d)", fmt, w, h);
}
bool tex_log = getenv("R6SV_TEXLOG") != nullptr;
u32 bound_tex() {
    s32 id = 0;
    g_GetIntegerv(0x8069, &id);  // GL_TEXTURE_BINDING_2D
    return (u32)id;
}
void h_CompressedTexImage2D(u32 t, s32 lvl, u32 fmt, s32 w, s32 h, s32 border, s32 size, const void* data) {
    if (tex_log && lvl == 0) LOGI("glCompressedTexImage2D tex=%x fmt=%04x %dx%d size=%d", bound_tex(), fmt, w, h, size);
    if (fmt >= 0x83F0 && fmt <= 0x83F3) {
        g_CompressedTexImage2D(t, lvl, fmt, w, h, border, size, data);
        ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha(fmt));
        return;
    }
    std::vector<u8> rgba;
    if (!data || !tex::decode_atc(fmt, (const u8*)data, (u32)w, (u32)h, rgba)) {
        unsupported_format(fmt, w, h);
        return;
    }
    // R6SV_DUMPTEX=id: save that texture's decoded level 0 as tex_<id>.bmp (and tex_<id>_alpha.bmp)
    static const char* dump = getenv("R6SV_DUMPTEX");
    if (dump && lvl == 0 && strtoul(dump, nullptr, 16) == bound_tex()) {
        for (int pass = 0; pass < 2; pass++) {
            char name[64];
            snprintf(name, sizeof name, pass ? "tex_%x_alpha.bmp" : "tex_%x.bmp", bound_tex());
            FILE* f = fopen(name, "wb");
            if (!f) continue;
            u32 px = (u32)w * h, file = 54 + px * 4;
            u8 hdr[54] = {'B', 'M'};
            memcpy(hdr + 2, &file, 4);
            hdr[10] = 54; hdr[14] = 40;
            s32 hh = -h;  // top-down
            memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &hh, 4);
            hdr[26] = 1; hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            for (u32 i = 0; i < px; i++) {
                const u8* c = &rgba[i * 4];
                u8 o[4] = {pass ? c[3] : c[2], pass ? c[3] : c[1], pass ? c[3] : c[0], 255};
                fwrite(o, 1, 4, f);
            }
            fclose(f);
        }
        LOGI("dumped texture %x (%dx%d, format %04x)", bound_tex(), w, h, fmt);
    }
    g_TexImage2D(t, lvl, 0x1908 /*RGBA*/, w, h, 0, 0x1908, 0x1401 /*UNSIGNED_BYTE*/, rgba.data());
    ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha(fmt));
}
void h_TexImage2D(u32 t, s32 lvl, s32 ifmt, s32 w, s32 h, s32 border, u32 fmt, u32 type, const void* data) {
    if (tex_log && lvl == 0) LOGI("glTexImage2D tex=%x ifmt=%04x fmt=%04x type=%04x %dx%d", bound_tex(), ifmt, fmt, type, w, h);
    g_TexImage2D(t, lvl, ifmt, w, h, border, fmt, type, data);
    ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha((u32)ifmt));
}
void h_CompressedTexSubImage2D(u32 t, s32 lvl, s32 x, s32 y, s32 w, s32 h, u32 fmt, s32 size, const void* data) {
    if (fmt >= 0x83F0 && fmt <= 0x83F3) {
        g_CompressedTexSubImage2D(t, lvl, x, y, w, h, fmt, size, data);
        return;
    }
    std::vector<u8> rgba;
    if (!data || !tex::decode_atc(fmt, (const u8*)data, (u32)w, (u32)h, rgba)) {
        unsupported_format(fmt, w, h);
        return;
    }
    g_TexSubImage2D(t, lvl, x, y, w, h, 0x1908, 0x1401, rgba.data());
}

// --- shaders ------------------------------------------------------------------------
std::string translate_glsl(const std::string& src) {
    static const std::regex drop(R"((^|\n)[ \t]*#[ \t]*(version|extension)[^\n]*)");
    static const std::regex precision(R"(precision\s+\w+\s+\w+\s*;)");
    std::string s = std::regex_replace(src, drop, "$1");
    s = std::regex_replace(s, precision, "");
    return "#version 120\n#define lowp\n#define mediump\n#define highp\n#line 1\n" + s;
}
void h_ShaderSource(u32 shader, s32 count, const u32* strings, const s32* lengths) {
    std::string src;
    for (s32 i = 0; i < count; i++) {
        const char* s = mem::str(strings[i]);
        if (lengths && lengths[i] >= 0) src.append(s, lengths[i]);
        else src += s;
    }
    if (gl_trace) {
        static int n = 0;
        CreateDirectoryA("shaders", nullptr);
        std::string name = "shaders/" + std::to_string(n++) + ".glsl";
        if (FILE* f = fopen(name.c_str(), "wb")) {
            fwrite(src.data(), 1, src.size(), f);
            fclose(f);
        }
        LOGI("GLTRACE shader %u -> %s", shader, name.c_str());
    }
    std::string out = translate_glsl(src);
    const char* p = out.c_str();
    g_ShaderSource(shader, 1, &p, nullptr);
}

void APIENTRY gl_debug(u32, u32 type, u32 id, u32 severity, s32, const char* msg, const void*) {
    if (severity == 0x826B) return;  // DEBUG_SEVERITY_NOTIFICATION
    static std::unordered_map<u32, int> seen;
    if (seen[id]++ < 3) LOGW("GL debug (type %04x): %s", type, msg);
}

// R6SV_GLTRACE also logs each distinct fixed-function state call (ES 1.1 path)
void(APIENTRY* t_Enable)(u32);
void(APIENTRY* t_Disable)(u32);
void(APIENTRY* t_TexEnvf)(u32, u32, float);
void(APIENTRY* t_TexEnvi)(u32, u32, s32);
void(APIENTRY* t_TexEnvfv)(u32, u32, const float*);
void(APIENTRY* t_Lightfv)(u32, u32, const float*);
void(APIENTRY* t_Lightf)(u32, u32, float);
void(APIENTRY* t_LightModelfv)(u32, const float*);
void(APIENTRY* t_LightModelf)(u32, float);
void(APIENTRY* t_Materialfv)(u32, u32, const float*);
void(APIENTRY* t_Materialf)(u32, u32, float);
void(APIENTRY* t_Color4f)(float, float, float, float);
void tr_Enable(u32 c) { trace_once("glEnable %04x", c); h_Enable(c); }
void tr_Disable(u32 c) { trace_once("glDisable %04x", c); h_Disable(c); }
void tr_TexEnvf(u32 t, u32 p, float v) { trace_once("glTexEnvf %04x %04x %g", t, p, v); h_TexEnvf(t, p, v); }
void tr_TexEnvi(u32 t, u32 p, s32 v) { trace_once("glTexEnvi %04x %04x %04x", t, p, v); h_TexEnvi(t, p, v); }
void tr_TexEnvfv(u32 t, u32 p, const float* v) { trace_once("glTexEnvfv %04x %04x %g %g %g %g", t, p, v[0], v[1], v[2], v[3]); h_TexEnvfv(t, p, v); }
void tr_Lightfv(u32 l, u32 p, const float* v) { trace_once("glLightfv %04x %04x %.2f %.2f %.2f %.2f", l, p, v[0], v[1], v[2], v[3]); t_Lightfv(l, p, v); }
void tr_Lightf(u32 l, u32 p, float v) { trace_once("glLightf %04x %04x %g", l, p, v); t_Lightf(l, p, v); }
void tr_LightModelfv(u32 p, const float* v) { trace_once("glLightModelfv %04x %g %g %g %g", p, v[0], v[1], v[2], v[3]); t_LightModelfv(p, v); }
void tr_LightModelf(u32 p, float v) { trace_once("glLightModelf %04x %g", p, v); t_LightModelf(p, v); }
void tr_Materialfv(u32 f, u32 p, const float* v) { trace_once("glMaterialfv %04x %04x %.2f %.2f %.2f %.2f", f, p, v[0], v[1], v[2], v[3]); t_Materialfv(f, p, v); }
void tr_Materialf(u32 f, u32 p, float v) { trace_once("glMaterialf %04x %04x %g", f, p, v); t_Materialf(f, p, v); }
void tr_Color4f(float r, float g, float b, float a) { trace_once("glColor4f %.2f %.2f %.2f %.2f", r, g, b, a); t_Color4f(r, g, b, a); }

u32 gstr(const char* s) {
    u32 g = mem::data_alloc((u32)strlen(s) + 1);
    strcpy((char*)mem::H(g), s);
    return g;
}

}  // namespace

void init_gl_desktop() {
    const char*(APIENTRY * host_str)(u32);
    HOST(host_str, "glGetString", const char*, u32);
    LOGI("GL host: %s / %s / %s", host_str(0x1F00), host_str(0x1F01), host_str(0x1F02));
    if (getenv("R6SV_GLDEBUG")) {
        void(APIENTRY * cb)(decltype(&gl_debug), const void*);
        void(APIENTRY * enable)(u32);
        HOST(cb, "glDebugMessageCallback", void, decltype(&gl_debug), const void*);
        HOST(enable, "glEnable", void, u32);
        if (cb) {
            enable(0x92E0);  // GL_DEBUG_OUTPUT
            enable(0x8242);  // GL_DEBUG_OUTPUT_SYNCHRONOUS
            cb(gl_debug, nullptr);
        }
    }
    // what the game sees: the Xperia Play's Adreno 205, minus formats a PC can't decode
    str_vendor = gstr("Qualcomm");
    str_renderer = gstr("Adreno 205");
    // R6SV_GLES1=1: claim ES 1.1 so the engine uses its fixed-function renderer
    str_version = gstr(getenv("R6SV_GLES1") ? "OpenGL ES-CM 1.1" : "OpenGL ES 2.0");
    str_glsl = gstr("OpenGL ES GLSL ES 1.00");
    str_ext = gstr("GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_texture_npot "
                   "GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_element_index_uint GL_OES_texture_3D "
                   "GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc GL_AMD_compressed_ATC_texture ");  // Glitch needs the trailing space

    HOST(g_BindBuffer, "glBindBuffer", void, u32, u32);
    HOST(g_BufferData, "glBufferData", void, u32, ptrdiff_t, const void*, u32);
    HOST(g_BufferSubData, "glBufferSubData", void, u32, ptrdiff_t, ptrdiff_t, const void*);
    HOST(g_GetBufferParameteriv, "glGetBufferParameteriv", void, u32, u32, s32*);
    HOST(g_VertexAttribPointer, "glVertexAttribPointer", void, u32, s32, u32, u8, s32, const void*);
    HOST(g_VertexPointer, "glVertexPointer", void, s32, u32, s32, const void*);
    HOST(g_ColorPointer, "glColorPointer", void, s32, u32, s32, const void*);
    HOST(g_TexCoordPointer, "glTexCoordPointer", void, s32, u32, s32, const void*);
    HOST(g_NormalPointer, "glNormalPointer", void, u32, s32, const void*);
    HOST(g_DrawElements, "glDrawElements", void, u32, s32, u32, const void*);
    HOST(g_ClearDepth, "glClearDepth", void, double);
    HOST(g_DepthRange, "glDepthRange", void, double, double);
    HOST(g_ClipPlane, "glClipPlane", void, u32, const double*);
    HOST(g_GetIntegerv, "glGetIntegerv", void, u32, s32*);
    HOST(g_ShaderSource, "glShaderSource", void, u32, s32, const char* const*, const s32*);
    HOST(g_CompressedTexImage2D, "glCompressedTexImage2D", void, u32, s32, u32, s32, s32, s32, s32, const void*);
    HOST(g_CompressedTexSubImage2D, "glCompressedTexSubImage2D", void, u32, s32, s32, s32, s32, s32, u32, s32, const void*);
    HOST(g_TexImage2D, "glTexImage2D", void, u32, s32, s32, s32, s32, s32, u32, u32, const void*);
    HOST(g_TexSubImage2D, "glTexSubImage2D", void, u32, s32, s32, s32, s32, s32, u32, u32, const void*);

#define R(name, fn) reg(name, W<fn>)
    R("glBindBuffer", h_BindBuffer); R("glBufferData", h_BufferData); R("glBufferSubData", h_BufferSubData);
    R("glVertexAttribPointer", h_VertexAttribPointer); R("glVertexPointer", h_VertexPointer);
    R("glColorPointer", h_ColorPointer); R("glTexCoordPointer", h_TexCoordPointer);
    R("glNormalPointer", h_NormalPointer); R("glDrawElements", h_DrawElements);
    R("glMapBufferOES", h_MapBuffer); R("glUnmapBufferOES", h_UnmapBuffer);
    R("glClearDepthf", h_ClearDepthf); R("glDepthRangef", h_DepthRangef); R("glClipPlanef", h_ClipPlanef);
    R("glGetString", h_GetString); R("glGetIntegerv", h_GetIntegerv); R("glShaderSource", h_ShaderSource);
    R("glCompressedTexImage2D", h_CompressedTexImage2D); R("glCompressedTexSubImage2D", h_CompressedTexSubImage2D);
#undef R

    using v = void;
    using f = float;
    using cvp = const void*;
    PASS("glActiveTexture", "glActiveTexture", v, u32);
    PASS("glAlphaFunc", "glAlphaFunc", v, u32, f);
    PASS("glAttachShader", "glAttachShader", v, u32, u32);
    PASS("glBindTexture", "glBindTexture", v, u32, u32);
    PASS("glBlendColor", "glBlendColor", v, f, f, f, f);
    PASS("glBlendEquation", "glBlendEquation", v, u32);
    PASS("glBlendEquationOES", "glBlendEquation", v, u32);
    PASS("glBlendFunc", "glBlendFunc", v, u32, u32);
    PASS("glClear", "glClear", v, u32);
    PASS("glClearColor", "glClearColor", v, f, f, f, f);
    PASS("glClearStencil", "glClearStencil", v, s32);
    PASS("glClientActiveTexture", "glClientActiveTexture", v, u32);
    PASS("glColor4f", "glColor4f", v, f, f, f, f);
    PASS("glColor4ub", "glColor4ub", v, u8, u8, u8, u8);
    PASS("glColorMask", "glColorMask", v, u8, u8, u8, u8);
    PASS("glCompileShader", "glCompileShader", v, u32);
    PASS("glCompressedTexImage3DOES", "glCompressedTexImage3D", v, u32, s32, u32, s32, s32, s32, s32, s32, cvp);
    PASS("glCompressedTexSubImage3DOES", "glCompressedTexSubImage3D", v, u32, s32, s32, s32, s32, s32, s32, s32, u32, s32, cvp);
    PASS("glCopyTexImage2D", "glCopyTexImage2D", v, u32, s32, u32, s32, s32, s32, s32, s32);
    PASS("glCopyTexSubImage2D", "glCopyTexSubImage2D", v, u32, s32, s32, s32, s32, s32, s32, s32);
    PASS("glCreateProgram", "glCreateProgram", u32);
    PASS("glCreateShader", "glCreateShader", u32, u32);
    PASS("glCullFace", "glCullFace", v, u32);
    PASS("glDeleteBuffers", "glDeleteBuffers", v, s32, const u32*);
    PASS("glDeleteFramebuffers", "glDeleteFramebuffers", v, s32, const u32*);
    PASS("glDeleteFramebuffersOES", "glDeleteFramebuffers", v, s32, const u32*);
    PASS("glDeleteProgram", "glDeleteProgram", v, u32);
    PASS("glDeleteRenderbuffers", "glDeleteRenderbuffers", v, s32, const u32*);
    PASS("glDeleteRenderbuffersOES", "glDeleteRenderbuffers", v, s32, const u32*);
    PASS("glDeleteShader", "glDeleteShader", v, u32);
    PASS("glDeleteTextures", "glDeleteTextures", v, s32, const u32*);
    PASS("glDepthFunc", "glDepthFunc", v, u32);
    PASS("glDepthMask", "glDepthMask", v, u8);
    PASS("glDisable", "glDisable", v, u32);
    PASS("glDisableClientState", "glDisableClientState", v, u32);
    PASS("glDisableVertexAttribArray", "glDisableVertexAttribArray", v, u32);
    PASS("glDrawArrays", "glDrawArrays", v, u32, s32, s32);
    PASS("glEnable", "glEnable", v, u32);
    PASS("glEnableClientState", "glEnableClientState", v, u32);
    PASS("glEnableVertexAttribArray", "glEnableVertexAttribArray", v, u32);
    PASS("glFlush", "glFlush", v);
    PASS("glFogf", "glFogf", v, u32, f);
    PASS("glFogfv", "glFogfv", v, u32, const f*);
    PASS("glFramebufferRenderbuffer", "glFramebufferRenderbuffer", v, u32, u32, u32, u32);
    PASS("glFramebufferRenderbufferOES", "glFramebufferRenderbuffer", v, u32, u32, u32, u32);
    PASS("glFramebufferTexture2D", "glFramebufferTexture2D", v, u32, u32, u32, u32, s32);
    PASS("glFramebufferTexture2DOES", "glFramebufferTexture2D", v, u32, u32, u32, u32, s32);
    PASS("glFrontFace", "glFrontFace", v, u32);
    PASS("glGenBuffers", "glGenBuffers", v, s32, u32*);
    PASS("glGenFramebuffers", "glGenFramebuffers", v, s32, u32*);
    PASS("glGenFramebuffersOES", "glGenFramebuffers", v, s32, u32*);
    PASS("glGenRenderbuffers", "glGenRenderbuffers", v, s32, u32*);
    PASS("glGenRenderbuffersOES", "glGenRenderbuffers", v, s32, u32*);
    PASS("glGenTextures", "glGenTextures", v, s32, u32*);
    PASS("glGenerateMipmap", "glGenerateMipmap", v, u32);
    PASS("glGenerateMipmapOES", "glGenerateMipmap", v, u32);
    PASS("glGetActiveAttrib", "glGetActiveAttrib", v, u32, u32, s32, s32*, s32*, u32*, char*);
    PASS("glGetActiveUniform", "glGetActiveUniform", v, u32, u32, s32, s32*, s32*, u32*, char*);
    PASS("glGetAttribLocation", "glGetAttribLocation", s32, u32, const char*);
    PASS("glGetError", "glGetError", u32);
    PASS("glGetFloatv", "glGetFloatv", v, u32, f*);
    PASS("glGetProgramInfoLog", "glGetProgramInfoLog", v, u32, s32, s32*, char*);
    PASS("glGetProgramiv", "glGetProgramiv", v, u32, u32, s32*);
    PASS("glGetShaderInfoLog", "glGetShaderInfoLog", v, u32, s32, s32*, char*);
    PASS("glGetShaderiv", "glGetShaderiv", v, u32, u32, s32*);
    PASS("glGetUniformLocation", "glGetUniformLocation", s32, u32, const char*);
    PASS("glHint", "glHint", v, u32, u32);
    PASS("glLightModelf", "glLightModelf", v, u32, f);
    PASS("glLightModelfv", "glLightModelfv", v, u32, const f*);
    PASS("glLightf", "glLightf", v, u32, u32, f);
    PASS("glLightfv", "glLightfv", v, u32, u32, const f*);
    PASS("glLineWidth", "glLineWidth", v, f);
    PASS("glLinkProgram", "glLinkProgram", v, u32);
    PASS("glLoadIdentity", "glLoadIdentity", v);
    PASS("glLoadMatrixf", "glLoadMatrixf", v, const f*);
    PASS("glMaterialf", "glMaterialf", v, u32, u32, f);
    PASS("glMaterialfv", "glMaterialfv", v, u32, u32, const f*);
    PASS("glMatrixMode", "glMatrixMode", v, u32);
    PASS("glMultMatrixf", "glMultMatrixf", v, const f*);
    PASS("glMultiTexCoord4f", "glMultiTexCoord4f", v, u32, f, f, f, f);
    PASS("glNormal3f", "glNormal3f", v, f, f, f);
    PASS("glPixelStorei", "glPixelStorei", v, u32, s32);
    PASS("glPointParameterf", "glPointParameterf", v, u32, f);
    PASS("glPointParameterfv", "glPointParameterfv", v, u32, const f*);
    PASS("glPointSize", "glPointSize", v, f);
    PASS("glPolygonOffset", "glPolygonOffset", v, f, f);
    PASS("glPopMatrix", "glPopMatrix", v);
    PASS("glPushMatrix", "glPushMatrix", v);
    PASS("glReadPixels", "glReadPixels", v, s32, s32, s32, s32, u32, u32, void*);
    PASS("glRenderbufferStorage", "glRenderbufferStorage", v, u32, u32, s32, s32);
    PASS("glRenderbufferStorageOES", "glRenderbufferStorage", v, u32, u32, s32, s32);
    PASS("glSampleCoverage", "glSampleCoverage", v, f, u8);
    PASS("glScissor", "glScissor", v, s32, s32, s32, s32);
    PASS("glShadeModel", "glShadeModel", v, u32);
    PASS("glStencilFunc", "glStencilFunc", v, u32, s32, u32);
    PASS("glStencilMask", "glStencilMask", v, u32);
    PASS("glStencilOp", "glStencilOp", v, u32, u32, u32);
    PASS("glTexEnvf", "glTexEnvf", v, u32, u32, f);
    PASS("glTexEnvfv", "glTexEnvfv", v, u32, u32, const f*);
    PASS("glTexEnvi", "glTexEnvi", v, u32, u32, s32);
    reg("glTexImage2D", W<h_TexImage2D>);
    PASS("glTexImage3DOES", "glTexImage3D", v, u32, s32, s32, s32, s32, s32, s32, u32, u32, cvp);
    PASS("glTexParameterf", "glTexParameterf", v, u32, u32, f);
    PASS("glTexParameteri", "glTexParameteri", v, u32, u32, s32);
    PASS("glTexSubImage2D", "glTexSubImage2D", v, u32, s32, s32, s32, s32, s32, u32, u32, cvp);
    PASS("glTexSubImage3DOES", "glTexSubImage3D", v, u32, s32, s32, s32, s32, s32, s32, s32, u32, u32, cvp);
    PASS("glUniform1f", "glUniform1f", v, s32, f);
    PASS("glUniform1fv", "glUniform1fv", v, s32, s32, const f*);
    PASS("glUniform1i", "glUniform1i", v, s32, s32);
    PASS("glUniform1iv", "glUniform1iv", v, s32, s32, const s32*);
    PASS("glUniform2fv", "glUniform2fv", v, s32, s32, const f*);
    PASS("glUniform2iv", "glUniform2iv", v, s32, s32, const s32*);
    PASS("glUniform3fv", "glUniform3fv", v, s32, s32, const f*);
    PASS("glUniform3iv", "glUniform3iv", v, s32, s32, const s32*);
    PASS("glUniform4fv", "glUniform4fv", v, s32, s32, const f*);
    PASS("glUniform4iv", "glUniform4iv", v, s32, s32, const s32*);
    PASS("glUniformMatrix4fv", "glUniformMatrix4fv", v, s32, s32, u8, const f*);
    PASS("glUseProgram", "glUseProgram", v, u32);
    PASS("glVertexAttrib4f", "glVertexAttrib4f", v, u32, f, f, f, f);
    PASS("glViewport", "glViewport", v, s32, s32, s32, s32);
    PASS("glBindFramebuffer", "glBindFramebuffer", v, u32, u32);
    PASS("glBindFramebufferOES", "glBindFramebuffer", v, u32, u32);
    PASS("glBindRenderbuffer", "glBindRenderbuffer", v, u32, u32);
    PASS("glBindRenderbufferOES", "glBindRenderbuffer", v, u32, u32);
    PASS("glCheckFramebufferStatus", "glCheckFramebufferStatus", u32, u32);
    PASS("glCheckFramebufferStatusOES", "glCheckFramebufferStatus", u32, u32);

    // state the ES 1.1 fix-ups track, and the draws they wrap
    HOST(g_ActiveTexture, "glActiveTexture", void, u32);
    HOST(g_BindTexture, "glBindTexture", void, u32, u32);
    HOST(g_Enable, "glEnable", void, u32);
    HOST(g_Disable, "glDisable", void, u32);
    HOST(g_TexEnvi, "glTexEnvi", void, u32, u32, s32);
    HOST(g_TexEnvf, "glTexEnvf", void, u32, u32, float);
    HOST(g_TexEnvfv, "glTexEnvfv", void, u32, u32, const float*);
    HOST(g_DrawArrays, "glDrawArrays", void, u32, s32, s32);
    reg("glActiveTexture", W<h_ActiveTexture>); reg("glBindTexture", W<h_BindTexture>);
    reg("glEnable", W<h_Enable>); reg("glDisable", W<h_Disable>);
    reg("glTexEnvi", W<h_TexEnvi>); reg("glTexEnvf", W<h_TexEnvf>); reg("glTexEnvfv", W<h_TexEnvfv>);
    reg("glDrawArrays", W<h_DrawArrays>);
    ffp_fix::set_host({[](u32 u) { g_ActiveTexture(u); }, [](u32 t, u32 p, s32 v) { g_TexEnvi(t, p, v); },
                       [](u32 t, u32 p, const float* v) { g_TexEnvfv(t, p, v); }});

    if (gl_trace) {
        HOST(t_Enable, "glEnable", void, u32);
        HOST(t_Disable, "glDisable", void, u32);
        HOST(t_TexEnvf, "glTexEnvf", void, u32, u32, float);
        HOST(t_TexEnvi, "glTexEnvi", void, u32, u32, s32);
        HOST(t_TexEnvfv, "glTexEnvfv", void, u32, u32, const float*);
        HOST(t_Lightfv, "glLightfv", void, u32, u32, const float*);
        HOST(t_Lightf, "glLightf", void, u32, u32, float);
        HOST(t_LightModelfv, "glLightModelfv", void, u32, const float*);
        HOST(t_LightModelf, "glLightModelf", void, u32, float);
        HOST(t_Materialfv, "glMaterialfv", void, u32, u32, const float*);
        HOST(t_Materialf, "glMaterialf", void, u32, u32, float);
        HOST(t_Color4f, "glColor4f", void, float, float, float, float);
        reg("glEnable", W<tr_Enable>); reg("glDisable", W<tr_Disable>);
        reg("glTexEnvf", W<tr_TexEnvf>); reg("glTexEnvi", W<tr_TexEnvi>); reg("glTexEnvfv", W<tr_TexEnvfv>);
        reg("glLightfv", W<tr_Lightfv>); reg("glLightf", W<tr_Lightf>);
        reg("glLightModelfv", W<tr_LightModelfv>); reg("glLightModelf", W<tr_LightModelf>);
        reg("glMaterialfv", W<tr_Materialfv>); reg("glMaterialf", W<tr_Materialf>);
        reg("glColor4f", W<tr_Color4f>);
    }
}

}  // namespace hle
