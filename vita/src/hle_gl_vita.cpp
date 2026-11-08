// GLES 1.1/2.0 -> vitaGL bridge. vitaGL speaks GLES with both the fixed-function
// and the shader API (GLSL is translated on the console), so most calls pass
// straight through. The ArgReader-based wrappers take the game's softfp float
// arguments from core registers and call vitaGL with the hard-float ABI.
//
// The game always believes it renders at 854x480 (other sizes switch it to the
// touch-screen HUD); viewport and scissor rectangles on the screen framebuffer
// are scaled up to the display.
#include <vitaGL.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <psp2/io/stat.h>

#include <map>

#include "ffp_fix.h"
#include "glsl_to_cg.h"
#include "hle.h"
#include "texture_decode.h"
#include "vita_screen.h"

namespace hle {
namespace {

// Fn<Id, R, A...> holds one vitaGL entry point and a wrapper the thunks can call.
template <int Id, class R, class... A> struct Fn {
    static inline R (*p)(A...) = nullptr;
    static R call(A... a) { return p(a...); }
};

// PASS(guest name, vitaGL function, return, args...) registers a straight pass-through.
#define PASS(gname, fn, R, ...)                    \
    {                                              \
        using F = Fn<__LINE__, R, ##__VA_ARGS__>;  \
        F::p = (decltype(F::p))&fn;                \
        reg(gname, W<&F::call>);                   \
    }

u32 str_vendor, str_renderer, str_version, str_glsl, str_ext;
u32 bound_fb = 0;

// --- screen scaling -------------------------------------------------------------
int sx(int v) { return (int)std::lround((double)v * screen::WIDTH / screen::LOGICAL_W); }
int sy(int v) { return (int)std::lround((double)v * screen::HEIGHT / screen::LOGICAL_H); }
int ux(int v) { return (int)std::lround((double)v * screen::LOGICAL_W / screen::WIDTH); }
int uy(int v) { return (int)std::lround((double)v * screen::LOGICAL_H / screen::HEIGHT); }

void h_BindFramebuffer(u32 target, u32 fb) {
    bound_fb = fb;
    glBindFramebuffer(target, fb);
}
void h_Viewport(s32 x, s32 y, s32 w, s32 h) {
    if (bound_fb) glViewport(x, y, w, h);
    else glViewport(sx(x), sy(y), sx(x + w) - sx(x), sy(y + h) - sy(y));
}
void h_Scissor(s32 x, s32 y, s32 w, s32 h) {
    if (bound_fb) glScissor(x, y, w, h);
    else glScissor(sx(x), sy(y), sx(x + w) - sx(x), sy(y + h) - sy(y));
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
    if (pname == 0x86A2) { *p = (s32)std::size(COMPRESSED); return; }  // NUM_COMPRESSED_TEXTURE_FORMATS
    if (pname == 0x86A3) { for (u32 f : COMPRESSED) *p++ = (s32)f; return; }
    glGetIntegerv(pname, p);
    if (!bound_fb && (pname == 0x0BA2 || pname == 0x0C10)) {  // VIEWPORT, SCISSOR_BOX: report logical size
        s32 x = p[0], y = p[1];
        p[0] = ux(x);
        p[1] = uy(y);
        p[2] = ux(x + p[2]) - p[0];
        p[3] = uy(y + p[3]) - p[1];
    }
}

// --- diagnostics ------------------------------------------------------------------
// Log each distinct texture upload shape once.
void trace_tex(const char* fn, u32 ifmt, u32 fmt, u32 type, s32 lvl) {
    static std::unordered_map<u64, int> seen;
    u64 key = ((u64)ifmt << 40) ^ ((u64)fmt << 24) ^ ((u64)type << 8) ^ (u64)(lvl > 0) ^ ((u64)fn[2] << 56);
    if (seen[key]++ == 0) LOGI("GL: %s ifmt=%04x fmt=%04x type=%04x %s", fn, ifmt, fmt, type, lvl > 0 ? "(mip level)" : "(base level)");
}

// --- textures ---------------------------------------------------------------------
void unsupported_format(u32 fmt, s32 w, s32 h) {
    static int n;
    if (n++ < 10) LOGW("GL: unsupported compressed texture format %04x (%dx%d)", fmt, w, h);
}
void h_CompressedTexImage2D(u32 t, s32 lvl, u32 fmt, s32 w, s32 h, s32 border, s32 size, const void* data) {
    trace_tex("glCompressedTexImage2D", fmt, 0, 0, lvl);
    if (fmt >= 0x83F0 && fmt <= 0x83F3) {
        glCompressedTexImage2D(t, lvl, fmt, w, h, border, size, data);
        ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha(fmt));
        return;
    }
    std::vector<u8> dxt;  // ATC that came from DXT5 data (the LG build's) goes back to the GPU as DXT5
    if (fmt == tex::ATC_RGBA_INTERPOLATED && data && tex::atc_to_dxt5((const u8*)data, (u32)size, dxt)) {
        glCompressedTexImage2D(t, lvl, 0x83F3, w, h, border, size, dxt.data());
        ffp_fix::tex_image((u32)lvl, true);
        return;
    }
    std::vector<u8> rgba;
    if (!data || !tex::decode_atc(fmt, (const u8*)data, (u32)w, (u32)h, rgba)) {
        unsupported_format(fmt, w, h);
        return;
    }
    glTexImage2D(t, lvl, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha(fmt));
}
void h_CompressedTexSubImage2D(u32 t, s32 lvl, s32 x, s32 y, s32 w, s32 h, u32 fmt, s32 size, const void* data) {
    if (fmt >= 0x83F0 && fmt <= 0x83F3) {  // vitaGL can't update part of a DXT texture
        unsupported_format(fmt, w, h);
        return;
    }
    std::vector<u8> rgba;
    if (!data || !tex::decode_atc(fmt, (const u8*)data, (u32)w, (u32)h, rgba)) {
        unsupported_format(fmt, w, h);
        return;
    }
    glTexSubImage2D(t, lvl, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

void h_TexImage2D(u32 t, s32 lvl, s32 ifmt, s32 w, s32 h, s32 border, u32 fmt, u32 type, const void* data) {
    trace_tex("glTexImage2D", (u32)ifmt, fmt, type, lvl);
    glTexImage2D(t, lvl, ifmt, w, h, border, fmt, type, data);
    ffp_fix::tex_image((u32)lvl, ffp_fix::format_has_alpha((u32)ifmt));
}

// --- ES 1.1 state the fixed-function fix-ups track (see ffp_fix.h) -------------------
void h_ActiveTexture(u32 u) { ffp_fix::active_texture(u); glActiveTexture(u); }
void h_BindTexture(u32 t, u32 tex) { ffp_fix::bind_texture(t, tex); glBindTexture(t, tex); }
void h_Enable(u32 cap) { ffp_fix::enable(cap, true); glEnable(cap); }
void h_Disable(u32 cap) { ffp_fix::enable(cap, false); glDisable(cap); }
void h_TexEnvi(u32 t, u32 p, s32 v) { ffp_fix::tex_envi(t, p, v); glTexEnvi(t, p, v); }
void h_TexEnvf(u32 t, u32 p, float v) { ffp_fix::tex_envi(t, p, (s32)v); glTexEnvf(t, p, v); }
void h_TexEnvfv(u32 t, u32 p, const float* v) { ffp_fix::tex_envfv(t, p, v); glTexEnvfv(t, p, (GLfloat*)v); }  // vitaGL's prototype isn't const
void h_DrawArrays(u32 mode, s32 first, s32 count) {
    bool fixed = ffp_fix::before_draw();
    glDrawArrays(mode, first, count);
    if (fixed) ffp_fix::after_draw();
}
void h_DrawElements(u32 mode, s32 count, u32 type, const void* p) {
    bool fixed = ffp_fix::before_draw();
    glDrawElements(mode, count, type, p);
    if (fixed) ffp_fix::after_draw();
}

// --- GLES calls vitaGL doesn't have -------------------------------------------------
void h_LightModelf(u32 pname, float v) { glLightModelfv(pname, &v); }
void h_Lightf(u32 light, u32 pname, float v) { glLightfv(light, pname, &v); }
void unsupported(Cpu& c) {
    static std::unordered_map<u32, int> seen;
    if (seen[cpu::current_svc()]++ == 0) LOGW("GL: %s is not supported on vitaGL, ignored", cpu::thunk_name(cpu::current_svc()));
    c.ret(0);
}

// Shader problems show up as broken or missing 3D, so log every failure.
void log_info(const char* what, u32 id, bool program) {
    char info[1024] = "";
    GLsizei n = 0;
    if (program) glGetProgramInfoLog(id, sizeof info - 1, &n, info);
    else glGetShaderInfoLog(id, sizeof info - 1, &n, info);
    static int count = 0;
    if (count++ < 40) LOGW("GL: %s %u failed: %s", what, id, info);
}
// --- shaders: GLSL ES -> Cg ---------------------------------------------------------
// vitaGL's built-in GLSL translator gets the engine's matrix math wrong, so the
// game's shaders are translated here (glsl_to_cg.cpp) and given to vitaGL as Cg.
// A program's vertex and fragment shader are translated together at link time,
// so their varyings get matching TEXCOORD semantics.
struct ShaderSrc {
    bool vertex = false;
    std::string glsl;
};
std::unordered_map<u32, ShaderSrc> shaders;
std::unordered_map<u32, std::vector<u32>> attached;  // program -> shaders
glsl::Options cg_options;
bool cg_dump = false;
int cg_failures = 0;

bool exists(const char* path) {
    if (FILE* f = fopen(path, "rb")) {
        fclose(f);
        return true;
    }
    return false;
}

void save_cg(u32 shader, const std::string& text) {
    sceIoMkdir("ux0:data/r6sv/cg", 0777);
    char path[64];
    snprintf(path, sizeof path, "ux0:data/r6sv/cg/%u.cg", shader);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

u32 h_CreateShader(u32 type) {
    bool vertex = type == GL_VERTEX_SHADER;
    u32 id = glCreateShader(vertex ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT);
    shaders[id].vertex = vertex;
    return id;
}
void h_ShaderSource(u32 shader, s32 count, const char* const* strings, const s32* lengths) {
    std::string& src = shaders[shader].glsl;
    src.clear();
    for (s32 i = 0; i < count; i++) {
        const char* s = strings[i];
        if (lengths && lengths[i] >= 0) src.append(s, (size_t)lengths[i]);
        else src += s;
    }
}
void h_CompileShader(u32) {}  // compiled at link time, once both halves are known
void h_GetShaderiv(u32 shader, u32 pname, s32* p) {
    auto it = shaders.find(shader);
    if (it == shaders.end()) return glGetShaderiv(shader, pname, p);
    switch (pname) {
    case GL_SHADER_TYPE: *p = it->second.vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER; return;
    case GL_COMPILE_STATUS: *p = GL_TRUE; return;
    case GL_INFO_LOG_LENGTH: *p = 0; return;
    case GL_SHADER_SOURCE_LENGTH: *p = (s32)it->second.glsl.size() + 1; return;
    default: glGetShaderiv(shader, pname, p);
    }
}
void h_GetShaderInfoLog(u32, s32 max, s32* len, char* log) {
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
// vitaGL's glAttachShader reads the compiled shader's parameters, and ours are
// only compiled at link time, so the real attach happens in h_LinkProgram.
std::unordered_map<u32, std::vector<u32>> gl_attached;  // program -> shaders attached in vitaGL
void h_AttachShader(u32 program, u32 shader) {
    std::vector<u32>& list = attached[program];
    if (std::find(list.begin(), list.end(), shader) == list.end()) list.push_back(shader);
}
void h_DetachShader(u32 program, u32 shader) {
    std::vector<u32>& list = attached[program];
    list.erase(std::remove(list.begin(), list.end(), shader), list.end());
    // vitaGL has no glDetachShader; the linked program keeps what it was linked with
}
void h_DeleteShader(u32 shader) {
    shaders.erase(shader);
    glDeleteShader(shader);
}
void h_DeleteProgram(u32 program) {
    attached.erase(program);
    gl_attached.erase(program);
    glDeleteProgram(program);
}

void h_LinkProgram(u32 program) {
    std::vector<u32>& list = attached[program];
    // varying slots: the vertex shader's order, then anything only the fragment shader has
    std::map<std::string, int> slots;
    for (int pass = 0; pass < 2; pass++)
        for (u32 s : list) {
            auto it = shaders.find(s);
            if (it != shaders.end() && it->second.vertex == (pass == 0))
                for (const std::string& v : glsl::varyings(it->second.glsl, it->second.vertex))
                    slots.emplace(v, (int)slots.size());
        }
    for (u32 s : list) {
        auto it = shaders.find(s);
        if (it == shaders.end()) continue;
        std::string cg, error;
        if (!glsl::to_cg(it->second.glsl, it->second.vertex, slots, cg_options, cg, error)) {
            LOGW("GL: shader %u can't be translated to Cg: %s", s, error.c_str());
            save_cg(s, "// " + error + "\n\n" + it->second.glsl);
            continue;
        }
        if (cg_dump) save_cg(s, cg);
        const char* p = cg.c_str();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 1;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            if (cg_failures++ < 20) {
                log_info("compiling Cg shader", s, false);
                save_cg(s, cg);  // ux0:data/r6sv/cg/<id>.cg, for the line numbers in the error
            }
            continue;  // attaching a shader that didn't compile crashes vitaGL
        }
        std::vector<u32>& real = gl_attached[program];
        if (std::find(real.begin(), real.end(), s) == real.end()) {
            glAttachShader(program, s);
            real.push_back(s);
        }
    }
    if (gl_attached[program].size() < 2) {
        LOGW("GL: program %u has no working vertex/fragment pair, not linked", program);
        return;
    }
    // vitaGL's interleaved-vertex path measures every attribute's offset from
    // attribute 0's, so attribute 0 must be the one at the lowest address: the
    // position. The lightmapped level shader declares its texcoords first.
    for (const char* name : {"Position", "Vertex"})
        if (glGetAttribLocation(program, name) > 0) {
            glBindAttribLocation(program, 0, name);
            break;
        }
    glLinkProgram(program);
    GLint ok = 1;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) log_info("linking program", program, true);
}

s32 h_GetUniformLocation(u32 program, const char* name) {
    return glGetUniformLocation(program, glsl::renamed_uniform(name).c_str());
}
void h_GetActiveUniform(u32 program, u32 index, s32 max, s32* len, s32* size, u32* type, char* name) {
    glGetActiveUniform(program, index, max, len, size, (GLenum*)type, name);
    // report Cg-keyword uniforms by their GLSL name ("texture_" -> "texture")
    if (name) {
        size_t n = strlen(name);
        if (n > 1 && name[n - 1] == '_' && glsl::renamed_uniform(std::string(name, n - 1)) == name) {
            name[n - 1] = 0;
            if (len) *len = (s32)n - 1;
        }
    }
}

u32 gstr(const char* s) {
    u32 g = mem::data_alloc((u32)strlen(s) + 1);
    strcpy((char*)mem::H(g), s);
    return g;
}

}  // namespace

void init_gl_vita() {
    LOGI("GL host: %s / %s / %s", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));
    // What the game sees: the Xperia Play's Adreno 205 with OpenGL ES 2.0; its
    // shaders are translated to Cg here. Files in ux0:data/r6sv/ change this:
    //   es1         claim OpenGL ES 1.1, so the engine uses its fixed-function renderer
    //   cg_dump     save every translated shader to ux0:data/r6sv/cg/
    //   cg_mul_alt  the other matrix multiplication order (if all 3D is garbled)
    const bool es1 = exists("ux0:data/r6sv/es1");
    cg_dump = exists("ux0:data/r6sv/cg_dump");
    cg_options.swap_mul = !exists("ux0:data/r6sv/cg_mul_alt");
    LOGI("GL: %s renderer%s%s", es1 ? "ES 1.1 (fixed-function)" : "ES 2.0 (shaders translated to Cg)",
         cg_dump ? ", dumping Cg" : "", cg_options.swap_mul ? "" : ", alternative matrix order");
    str_vendor = gstr("Qualcomm");
    str_renderer = gstr("Adreno 205");
    str_version = gstr(es1 ? "OpenGL ES-CM 1.1" : "OpenGL ES 2.0");
    str_glsl = gstr("OpenGL ES GLSL ES 1.00");
    str_ext = gstr("GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_texture_npot "
                   "GL_OES_framebuffer_object GL_OES_mapbuffer GL_OES_element_index_uint "
                   "GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc GL_AMD_compressed_ATC_texture ");  // Glitch needs the trailing space

#define R(name, fn) reg(name, W<fn>)
    R("glBindFramebuffer", h_BindFramebuffer); R("glBindFramebufferOES", h_BindFramebuffer);
    R("glViewport", h_Viewport); R("glScissor", h_Scissor);
    R("glGetString", h_GetString); R("glGetIntegerv", h_GetIntegerv);
    R("glCompressedTexImage2D", h_CompressedTexImage2D); R("glCompressedTexSubImage2D", h_CompressedTexSubImage2D);
    R("glLightModelf", h_LightModelf); R("glLightf", h_Lightf);
    R("glCompileShader", h_CompileShader); R("glLinkProgram", h_LinkProgram);
    R("glTexImage2D", h_TexImage2D);
#undef R
    for (const char* n : {"glTexImage3DOES", "glTexSubImage3DOES", "glCompressedTexImage3DOES",
                          "glCompressedTexSubImage3DOES", "glPointParameterf", "glPointParameterfv",
                          "glSampleCoverage", "glBlendColor", "glMultiTexCoord4f"})
        reg(n, unsupported);

    using v = void;
    using f = float;
    using cvp = const void*;
    PASS("glActiveTexture", glActiveTexture, v, u32);
    PASS("glAlphaFunc", glAlphaFunc, v, u32, f);
    PASS("glAttachShader", glAttachShader, v, u32, u32);
    PASS("glBindBuffer", glBindBuffer, v, u32, u32);
    PASS("glBindTexture", glBindTexture, v, u32, u32);
    PASS("glBlendEquation", glBlendEquation, v, u32);
    PASS("glBlendEquationOES", glBlendEquation, v, u32);
    PASS("glBlendFunc", glBlendFunc, v, u32, u32);
    PASS("glBufferData", glBufferData, v, u32, s32, cvp, u32);
    PASS("glBufferSubData", glBufferSubData, v, u32, s32, s32, cvp);
    PASS("glClear", glClear, v, u32);
    PASS("glClearColor", glClearColor, v, f, f, f, f);
    PASS("glClearDepthf", glClearDepthf, v, f);
    PASS("glClearStencil", glClearStencil, v, s32);
    PASS("glClientActiveTexture", glClientActiveTexture, v, u32);
    PASS("glClipPlanef", glClipPlanef, v, u32, const f*);
    PASS("glColor4f", glColor4f, v, f, f, f, f);
    PASS("glColor4ub", glColor4ub, v, u8, u8, u8, u8);
    PASS("glColorMask", glColorMask, v, u8, u8, u8, u8);
    PASS("glColorPointer", glColorPointer, v, s32, u32, s32, cvp);
    PASS("glCopyTexImage2D", glCopyTexImage2D, v, u32, s32, u32, s32, s32, s32, s32, s32);
    PASS("glCopyTexSubImage2D", glCopyTexSubImage2D, v, u32, s32, s32, s32, s32, s32, s32, s32);
    PASS("glCreateProgram", glCreateProgram, u32);
    PASS("glCreateShader", glCreateShader, u32, u32);
    PASS("glCullFace", glCullFace, v, u32);
    PASS("glDeleteBuffers", glDeleteBuffers, v, s32, const u32*);
    PASS("glDeleteFramebuffers", glDeleteFramebuffers, v, s32, const u32*);
    PASS("glDeleteFramebuffersOES", glDeleteFramebuffers, v, s32, const u32*);
    PASS("glDeleteProgram", glDeleteProgram, v, u32);
    PASS("glDeleteRenderbuffers", glDeleteRenderbuffers, v, s32, const u32*);
    PASS("glDeleteRenderbuffersOES", glDeleteRenderbuffers, v, s32, const u32*);
    PASS("glDeleteShader", glDeleteShader, v, u32);
    PASS("glDeleteTextures", glDeleteTextures, v, s32, const u32*);
    PASS("glDepthFunc", glDepthFunc, v, u32);
    PASS("glDepthMask", glDepthMask, v, u8);
    PASS("glDepthRangef", glDepthRangef, v, f, f);
    PASS("glDisable", glDisable, v, u32);
    PASS("glDisableClientState", glDisableClientState, v, u32);
    PASS("glDisableVertexAttribArray", glDisableVertexAttribArray, v, u32);
    PASS("glDrawArrays", glDrawArrays, v, u32, s32, s32);
    PASS("glDrawElements", glDrawElements, v, u32, s32, u32, cvp);
    PASS("glEnable", glEnable, v, u32);
    PASS("glEnableClientState", glEnableClientState, v, u32);
    PASS("glEnableVertexAttribArray", glEnableVertexAttribArray, v, u32);
    PASS("glFlush", glFlush, v);
    PASS("glFogf", glFogf, v, u32, f);
    PASS("glFogfv", glFogfv, v, u32, const f*);
    PASS("glFramebufferRenderbuffer", glFramebufferRenderbuffer, v, u32, u32, u32, u32);
    PASS("glFramebufferRenderbufferOES", glFramebufferRenderbuffer, v, u32, u32, u32, u32);
    PASS("glFramebufferTexture2D", glFramebufferTexture2D, v, u32, u32, u32, u32, s32);
    PASS("glFramebufferTexture2DOES", glFramebufferTexture2D, v, u32, u32, u32, u32, s32);
    PASS("glFrontFace", glFrontFace, v, u32);
    PASS("glGenBuffers", glGenBuffers, v, s32, u32*);
    PASS("glGenFramebuffers", glGenFramebuffers, v, s32, u32*);
    PASS("glGenFramebuffersOES", glGenFramebuffers, v, s32, u32*);
    PASS("glGenRenderbuffers", glGenRenderbuffers, v, s32, u32*);
    PASS("glGenRenderbuffersOES", glGenRenderbuffers, v, s32, u32*);
    PASS("glGenTextures", glGenTextures, v, s32, u32*);
    PASS("glGenerateMipmap", glGenerateMipmap, v, u32);
    PASS("glGenerateMipmapOES", glGenerateMipmap, v, u32);
    PASS("glGetActiveAttrib", glGetActiveAttrib, v, u32, u32, s32, s32*, s32*, u32*, char*);
    PASS("glGetAttribLocation", glGetAttribLocation, s32, u32, const char*);
    PASS("glGetProgramiv", glGetProgramiv, v, u32, u32, s32*);
    PASS("glUseProgram", glUseProgram, v, u32);
    PASS("glVertexAttribPointer", glVertexAttribPointer, v, u32, s32, u32, u8, s32, cvp);
    PASS("glGetActiveUniform", glGetActiveUniform, v, u32, u32, s32, s32*, s32*, u32*, char*);
    PASS("glGetError", glGetError, u32);
    PASS("glGetFloatv", glGetFloatv, v, u32, f*);
    PASS("glGetProgramInfoLog", glGetProgramInfoLog, v, u32, s32, s32*, char*);
    PASS("glGetShaderInfoLog", glGetShaderInfoLog, v, u32, s32, s32*, char*);
    PASS("glGetShaderiv", glGetShaderiv, v, u32, u32, s32*);
    PASS("glGetUniformLocation", glGetUniformLocation, s32, u32, const char*);
    PASS("glHint", glHint, v, u32, u32);
    PASS("glLightModelfv", glLightModelfv, v, u32, const f*);
    PASS("glLightfv", glLightfv, v, u32, u32, const f*);
    PASS("glLineWidth", glLineWidth, v, f);
    PASS("glLoadIdentity", glLoadIdentity, v);
    PASS("glLoadMatrixf", glLoadMatrixf, v, const f*);
    PASS("glMapBufferOES", glMapBuffer, void*, u32, u32);
    PASS("glMaterialf", glMaterialf, v, u32, u32, f);
    PASS("glMaterialfv", glMaterialfv, v, u32, u32, const f*);
    PASS("glMatrixMode", glMatrixMode, v, u32);
    PASS("glMultMatrixf", glMultMatrixf, v, const f*);
    PASS("glNormal3f", glNormal3f, v, f, f, f);
    PASS("glNormalPointer", glNormalPointer, v, u32, s32, cvp);
    PASS("glPixelStorei", glPixelStorei, v, u32, s32);
    PASS("glPointSize", glPointSize, v, f);
    PASS("glPolygonOffset", glPolygonOffset, v, f, f);
    PASS("glPopMatrix", glPopMatrix, v);
    PASS("glPushMatrix", glPushMatrix, v);
    PASS("glReadPixels", glReadPixels, v, s32, s32, s32, s32, u32, u32, void*);
    PASS("glRenderbufferStorage", glRenderbufferStorage, v, u32, u32, s32, s32);
    PASS("glRenderbufferStorageOES", glRenderbufferStorage, v, u32, u32, s32, s32);
    PASS("glShadeModel", glShadeModel, v, u32);
    PASS("glShaderSource", glShaderSource, v, u32, s32, const char* const*, const s32*);
    PASS("glStencilFunc", glStencilFunc, v, u32, s32, u32);
    PASS("glStencilMask", glStencilMask, v, u32);
    PASS("glStencilOp", glStencilOp, v, u32, u32, u32);
    PASS("glTexCoordPointer", glTexCoordPointer, v, s32, u32, s32, cvp);
    PASS("glTexEnvf", glTexEnvf, v, u32, u32, f);
    PASS("glTexEnvfv", glTexEnvfv, v, u32, u32, const f*);
    PASS("glTexEnvi", glTexEnvi, v, u32, u32, s32);
    PASS("glTexParameterf", glTexParameterf, v, u32, u32, f);
    PASS("glTexParameteri", glTexParameteri, v, u32, u32, s32);
    PASS("glTexSubImage2D", glTexSubImage2D, v, u32, s32, s32, s32, s32, s32, u32, u32, cvp);
    PASS("glUniform1f", glUniform1f, v, s32, f);
    PASS("glUniform1fv", glUniform1fv, v, s32, s32, const f*);
    PASS("glUniform1i", glUniform1i, v, s32, s32);
    PASS("glUniform1iv", glUniform1iv, v, s32, s32, const s32*);
    PASS("glUniform2fv", glUniform2fv, v, s32, s32, const f*);
    PASS("glUniform2iv", glUniform2iv, v, s32, s32, const s32*);
    PASS("glUniform3fv", glUniform3fv, v, s32, s32, const f*);
    PASS("glUniform3iv", glUniform3iv, v, s32, s32, const s32*);
    PASS("glUniform4fv", glUniform4fv, v, s32, s32, const f*);
    PASS("glUniform4iv", glUniform4iv, v, s32, s32, const s32*);
    PASS("glUniformMatrix4fv", glUniformMatrix4fv, v, s32, s32, u8, const f*);
    PASS("glUnmapBufferOES", glUnmapBuffer, u8, u32);
    PASS("glVertexAttrib4f", glVertexAttrib4f, v, u32, f, f, f, f);
    PASS("glVertexPointer", glVertexPointer, v, s32, u32, s32, cvp);
    PASS("glBindRenderbuffer", glBindRenderbuffer, v, u32, u32);
    PASS("glBindRenderbufferOES", glBindRenderbuffer, v, u32, u32);
    PASS("glCheckFramebufferStatus", glCheckFramebufferStatus, u32, u32);
    PASS("glCheckFramebufferStatusOES", glCheckFramebufferStatus, u32, u32);

    // shaders (replacing the pass-throughs)
    reg("glCreateShader", W<h_CreateShader>); reg("glShaderSource", W<h_ShaderSource>);
    reg("glCompileShader", W<h_CompileShader>); reg("glGetShaderiv", W<h_GetShaderiv>);
    reg("glGetShaderInfoLog", W<h_GetShaderInfoLog>); reg("glAttachShader", W<h_AttachShader>);
    reg("glDeleteShader", W<h_DeleteShader>); reg("glDetachShader", W<h_DetachShader>); reg("glDeleteProgram", W<h_DeleteProgram>);
    reg("glLinkProgram", W<h_LinkProgram>); reg("glGetUniformLocation", W<h_GetUniformLocation>);
    reg("glGetActiveUniform", W<h_GetActiveUniform>);

    // state the ES 1.1 fix-ups track, and the draws they wrap (replacing the pass-throughs)
    reg("glActiveTexture", W<h_ActiveTexture>); reg("glBindTexture", W<h_BindTexture>);
    reg("glEnable", W<h_Enable>); reg("glDisable", W<h_Disable>);
    reg("glTexEnvi", W<h_TexEnvi>); reg("glTexEnvf", W<h_TexEnvf>); reg("glTexEnvfv", W<h_TexEnvfv>);
    reg("glDrawArrays", W<h_DrawArrays>); reg("glDrawElements", W<h_DrawElements>);
    ffp_fix::set_host({[](u32 u) { glActiveTexture(u); }, [](u32 t, u32 p, s32 v) { glTexEnvi(t, p, v); },
                       [](u32 t, u32 p, const float* v) { glTexEnvfv(t, p, (GLfloat*)v); }});
}

}  // namespace hle
