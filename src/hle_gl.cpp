// Null GLES renderer: accepts every GL call the game makes and returns
// plausible state, so the game can boot headless. The real OpenGL bridge
// (SDL window) will replace these registrations.
#include <atomic>

#include "hle.h"

namespace hle {
namespace {

const char* const GL_NAMES[] = {
    "glActiveTexture", "glAlphaFunc", "glAttachShader", "glBindBuffer", "glBindFramebuffer",
    "glBindFramebufferOES", "glBindRenderbuffer", "glBindRenderbufferOES", "glBindTexture", "glBlendColor",
    "glBlendEquation", "glBlendEquationOES", "glBlendFunc", "glBufferData", "glBufferSubData", "glClear",
    "glClearColor", "glClearDepthf", "glClearStencil", "glClientActiveTexture", "glClipPlanef", "glColor4f",
    "glColor4ub", "glColorMask", "glColorPointer", "glCompileShader", "glCompressedTexImage2D",
    "glCompressedTexImage3DOES", "glCompressedTexSubImage2D", "glCompressedTexSubImage3DOES", "glCopyTexImage2D",
    "glCopyTexSubImage2D", "glCullFace", "glDeleteBuffers", "glDeleteFramebuffers", "glDeleteFramebuffersOES",
    "glDeleteProgram", "glDeleteRenderbuffers", "glDeleteRenderbuffersOES", "glDeleteShader", "glDeleteTextures",
    "glDepthFunc", "glDepthMask", "glDepthRangef", "glDisable", "glDisableClientState",
    "glDisableVertexAttribArray", "glDrawArrays", "glDrawElements", "glEnable", "glEnableClientState",
    "glEnableVertexAttribArray", "glFlush", "glFogf", "glFogfv", "glFramebufferRenderbuffer",
    "glFramebufferRenderbufferOES", "glFramebufferTexture2D", "glFramebufferTexture2DOES", "glFrontFace",
    "glGenerateMipmap", "glGenerateMipmapOES", "glGetActiveAttrib", "glGetActiveUniform", "glHint",
    "glLightModelf", "glLightModelfv", "glLightf", "glLightfv", "glLineWidth", "glLinkProgram", "glLoadIdentity",
    "glLoadMatrixf", "glMaterialf", "glMaterialfv", "glMatrixMode", "glMultMatrixf", "glMultiTexCoord4f",
    "glNormal3f", "glNormalPointer", "glPixelStorei", "glPointParameterf", "glPointParameterfv", "glPointSize",
    "glPolygonOffset", "glPopMatrix", "glPushMatrix", "glReadPixels", "glRenderbufferStorage",
    "glRenderbufferStorageOES", "glSampleCoverage", "glScissor", "glShadeModel", "glShaderSource",
    "glStencilFunc", "glStencilMask", "glStencilOp", "glTexCoordPointer", "glTexEnvf", "glTexEnvfv", "glTexEnvi",
    "glTexImage2D", "glTexImage3DOES", "glTexParameterf", "glTexParameteri", "glTexSubImage2D",
    "glTexSubImage3DOES", "glUniform1f", "glUniform1fv", "glUniform1i", "glUniform1iv", "glUniform2fv",
    "glUniform2iv", "glUniform3fv", "glUniform3iv", "glUniform4fv", "glUniform4iv", "glUniformMatrix4fv",
    "glUseProgram", "glVertexAttrib4f", "glVertexAttribPointer", "glVertexPointer", "glViewport",
};

std::atomic<u32> next_name{1};
u32 str_vendor, str_renderer, str_version, str_glsl, str_ext, map_scratch;

void noop(Cpu&) {}
void gen_names(s32 n, u32* out) {
    for (s32 i = 0; i < n; i++) out[i] = next_name++;
}
u32 new_name() { return next_name++; }
u32 h_glGetString(u32 name) {
    switch (name) {
    case 0x1F00: return str_vendor;
    case 0x1F01: return str_renderer;
    case 0x1F02: return str_version;
    case 0x1F03: return str_ext;
    case 0x8B8C: return str_glsl;
    }
    return 0;
}
void h_glGetIntegerv(u32 pname, s32* p) {
    switch (pname) {
    case 0x0D33: case 0x84E8: p[0] = 2048; break;    // MAX_TEXTURE_SIZE, MAX_RENDERBUFFER_SIZE
    case 0x84E2: p[0] = 4; break;                    // MAX_TEXTURE_UNITS
    case 0x8869: p[0] = 16; break;                   // MAX_VERTEX_ATTRIBS
    case 0x8872: case 0x8B4C: p[0] = 8; break;       // MAX_(VERTEX_)TEXTURE_IMAGE_UNITS
    case 0x8DFB: p[0] = 256; break;                  // MAX_VERTEX_UNIFORM_VECTORS
    case 0x8DFD: p[0] = 64; break;                   // MAX_FRAGMENT_UNIFORM_VECTORS
    case 0x8DFC: p[0] = 8; break;                    // MAX_VARYING_VECTORS
    case 0x0BA2: p[0] = p[1] = 0; p[2] = 854; p[3] = 480; break;  // VIEWPORT
    case 0x0D50: case 0x0D51: case 0x0D52: case 0x0D53: p[0] = 8; break;  // R/G/B/A bits
    case 0x0D56: p[0] = 24; break;                   // DEPTH_BITS
    default: p[0] = 0; break;
    }
}
void h_glGetFloatv(u32, float* p) { p[0] = 0; }
void h_glGetShaderiv(u32, u32 pname, s32* p) { *p = pname == 0x8B81 ? 1 : 0; }   // COMPILE_STATUS
void h_glGetProgramiv(u32, u32 pname, s32* p) { *p = pname == 0x8B82 ? 1 : 0; }  // LINK_STATUS
void h_glGetInfoLog(u32, s32 max, s32* len, char* log) {
    if (len) *len = 0;
    if (log && max > 0) log[0] = 0;
}
s32 h_glGetLocation(u32, const char*) { return (s32)new_name(); }
u32 h_glCheckFramebufferStatus(u32) { return 0x8CD5; }
u32 h_glGetError() { return 0; }
u32 h_glMapBuffer(u32, u32) { return map_scratch; }
u32 h_glUnmapBuffer(u32) { return 1; }

u32 gstr(const char* s) {
    u32 g = mem::data_alloc((u32)strlen(s) + 1);
    strcpy((char*)mem::H(g), s);
    return g;
}

}  // namespace

void init_gl_null() {
    str_vendor = gstr("Qualcomm");
    str_renderer = gstr("Adreno 205");
    str_version = gstr("OpenGL ES 2.0");
    str_glsl = gstr("OpenGL ES GLSL ES 1.00");
    str_ext = gstr("GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_texture_npot "
                   "GL_OES_framebuffer_object GL_OES_mapbuffer GL_EXT_texture_compression_dxt1 "
                   "GL_EXT_texture_compression_s3tc ");
    map_scratch = mem::alloc(8 << 20);

    for (const char* n : GL_NAMES) reg(n, noop);
#define R(name, fn) reg(name, W<fn>)
    R("glGetString", h_glGetString); R("glGetIntegerv", h_glGetIntegerv); R("glGetFloatv", h_glGetFloatv);
    R("glGenTextures", gen_names); R("glGenBuffers", gen_names); R("glGenFramebuffers", gen_names);
    R("glGenFramebuffersOES", gen_names); R("glGenRenderbuffers", gen_names); R("glGenRenderbuffersOES", gen_names);
    R("glCreateShader", new_name); R("glCreateProgram", new_name);
    R("glGetShaderiv", h_glGetShaderiv); R("glGetProgramiv", h_glGetProgramiv);
    R("glGetShaderInfoLog", h_glGetInfoLog); R("glGetProgramInfoLog", h_glGetInfoLog);
    R("glGetUniformLocation", h_glGetLocation); R("glGetAttribLocation", h_glGetLocation);
    R("glCheckFramebufferStatus", h_glCheckFramebufferStatus); R("glCheckFramebufferStatusOES", h_glCheckFramebufferStatus);
    R("glGetError", h_glGetError); R("glMapBufferOES", h_glMapBuffer); R("glUnmapBufferOES", h_glUnmapBuffer);
#undef R
}

}  // namespace hle
