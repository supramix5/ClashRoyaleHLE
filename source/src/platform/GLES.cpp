#include "platform/GLES.h"
#include "platform/Window.h"
#include "Hle.h"
#include "Log.h"
#include "Memory.h"
#include "libc/System.h"
#include "platform/GL.h"
#include "platform/Platform.h"
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>

using hle::arg;
using hle::ret;

namespace {
// Desktop GL constants beyond GL 1.1.
constexpr GLenum FRAMEBUFFER = 0x8D40, RENDERBUFFER = 0x8D41, READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9;
constexpr GLenum FRAMEBUFFER_BINDING = 0x8CA6, RENDERBUFFER_BINDING = 0x8CA7, COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, ARRAY_BUFFER_BINDING = 0x8894, ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
constexpr GLenum RGBA8 = 0x8058;

void *proc(const char *name) { return platform::glProc(name); }
template <class F> F fn(const char *name) {
    static std::unordered_map<std::string, void *> cache;
    static std::mutex lock;
    std::lock_guard guard(lock);
    auto &slot = cache[name];
    if (!slot) slot = proc(name);
    if (!slot) fatal("missing OpenGL entry point %s", name);
    return reinterpret_cast<F>(slot);
}
#define GLF(ret_, name, ...) fn<ret_(APIENTRY *)(__VA_ARGS__)>(#name)
const void *P(std::uint64_t a) { return a ? mem::base() + a : nullptr; }
void *PW(std::uint64_t a) { return a ? mem::base() + a : nullptr; }
float F(int i) { return cpu::s(i); }
thread_local bool t_hasContext = false;
std::uint32_t g_drawableRenderbuffer = 0, g_presentFbo = 0;
int g_drawableWidth = 0, g_drawableHeight = 0;
GLint binding(GLenum which) {
    GLint v = 0;
    glGetIntegerv(which, &v);
    return v;
}
void checkError(const char *what) {
    static const bool debug = std::getenv("HLE_GLDEBUG") != nullptr;
    if (!debug) return;
    const GLenum e = glGetError();
    if (e) hlog("gl", "%s -> error 0x%x", what, e);
}
// GLSL ES 1.00 -> GLSL 1.20: drop #version / precision statements, neutralise precision qualifiers.
std::string translateShader(const std::string &source) {
    std::string out = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    std::size_t start = 0;
    while (start < source.size()) {
        auto end = source.find('\n', start);
        if (end == std::string::npos) end = source.size();
        std::string line = source.substr(start, end - start);
        std::size_t first = line.find_first_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? "" : line.substr(first);
        if (trimmed.rfind("#version", 0) == 0) line.clear();
        else if (trimmed.rfind("precision ", 0) == 0) line.clear();
        else if (trimmed.rfind("#extension", 0) == 0) line.clear();
        out += line + "\n";
        start = end + 1;
    }
    return out;
}
} // namespace

namespace gles {
void init() {
    t_hasContext = true;
    hlog("gl", "GL_VERSION %s", reinterpret_cast<const char *>(glGetString(GL_VERSION)));
}
void ensureContext() {
    if (t_hasContext) return;
    platform::makeSharedContextCurrent();
    t_hasContext = true;
    hlog("gl", "shared GL context created for a background thread");
}
bool storageFromDrawable(int width, int height) {
    ensureContext();
    g_drawableRenderbuffer = std::uint32_t(binding(RENDERBUFFER_BINDING));
    g_drawableWidth = width;
    g_drawableHeight = height;
    GLF(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)(RENDERBUFFER, RGBA8, width, height);
    hlog("gl", "drawable renderbuffer %u is %dx%d", g_drawableRenderbuffer, width, height);
    return true;
}
std::string g_pendingShot;
std::function<void(int, int)> g_overlay;
void setOverlay(std::function<void(int, int)> draw) { g_overlay = std::move(draw); }
void requestScreenshot(const std::string &path) { g_pendingShot = path; }
bool present() {
    if (!g_drawableRenderbuffer) return false;
    const GLint read = binding(0x8CAA /*READ_FRAMEBUFFER_BINDING*/), draw = binding(FRAMEBUFFER_BINDING);
    if (!g_presentFbo) GLF(void, glGenFramebuffers, GLsizei, GLuint *)(1, &g_presentFbo);
    GLF(void, glBindFramebuffer, GLenum, GLuint)(READ_FRAMEBUFFER, g_presentFbo);
    GLF(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, RENDERBUFFER, g_drawableRenderbuffer);
    GLF(void, glBindFramebuffer, GLenum, GLuint)(DRAW_FRAMEBUFFER, 0);
    int w, h;
    platform::clientSize(w, h);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    GLF(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
    (0, 0, g_drawableWidth, g_drawableHeight, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (g_overlay) {
        GLint program = 0;
        glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &program);
        GLF(void, glBindFramebuffer, GLenum, GLuint)(0x8D40, 0);
        GLF(void, glUseProgram, GLuint)(0);
        g_overlay(w, h);
        GLF(void, glUseProgram, GLuint)(GLuint(program));
    }
    if (scissor) glEnable(GL_SCISSOR_TEST);
    // HLE_SHOT_WINDOW=1: screenshots show the window (game frame scaled, plus overlays) instead of the drawable.
    if (!g_pendingShot.empty() && std::getenv("HLE_SHOT_WINDOW")) {
        std::vector<unsigned char> pixels(std::size_t(w) * std::size_t(h) * 4);
        GLF(void, glBindFramebuffer, GLenum, GLuint)(READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, 0x80E1, GL_UNSIGNED_BYTE, pixels.data());
        if (FILE *f = std::fopen(g_pendingShot.c_str(), "wb")) {
            const std::uint32_t size = std::uint32_t(54 + pixels.size());
            unsigned char header[54] = {'B', 'M'};
            std::memcpy(header + 2, &size, 4);
            header[10] = 54; header[14] = 40;
            std::memcpy(header + 18, &w, 4);
            std::memcpy(header + 22, &h, 4);
            header[26] = 1; header[28] = 32;
            std::fwrite(header, 1, 54, f);
            std::fwrite(pixels.data(), 1, pixels.size(), f);
            std::fclose(f);
        }
        g_pendingShot.clear();
    }
    platform::swapBuffers();
    GLF(void, glBindFramebuffer, GLenum, GLuint)(READ_FRAMEBUFFER, GLuint(read));
    GLF(void, glBindFramebuffer, GLenum, GLuint)(DRAW_FRAMEBUFFER, GLuint(draw));
    static int frames = 0;
    if (++frames == 1 || frames % 600 == 0) hlog("gl", "presented frame %d", frames);
    // HLE_SCREENSHOT=<frame>:<path.bmp> saves the drawable once that frame is presented.
    static const char *shot = std::getenv("HLE_SCREENSHOT");
    std::string path;
    if (shot && frames == std::atoi(shot)) path = std::strchr(shot, ':') ? std::strchr(shot, ':') + 1 : "frame.bmp";
    if (!g_pendingShot.empty()) { path = g_pendingShot; g_pendingShot.clear(); }
    if (!path.empty()) {
        std::vector<unsigned char> pixels(std::size_t(g_drawableWidth) * std::size_t(g_drawableHeight) * 4);
        GLF(void, glBindFramebuffer, GLenum, GLuint)(READ_FRAMEBUFFER, g_presentFbo);
        glReadPixels(0, 0, g_drawableWidth, g_drawableHeight, 0x80E1 /*BGRA*/, GL_UNSIGNED_BYTE, pixels.data());
        GLF(void, glBindFramebuffer, GLenum, GLuint)(READ_FRAMEBUFFER, GLuint(read));
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
            const std::uint32_t size = std::uint32_t(54 + pixels.size());
            unsigned char header[54] = {'B', 'M'};
            std::memcpy(header + 2, &size, 4);
            header[10] = 54; header[14] = 40;
            std::memcpy(header + 18, &g_drawableWidth, 4);
            std::memcpy(header + 22, &g_drawableHeight, 4);
            header[26] = 1; header[28] = 32;
            std::fwrite(header, 1, 54, f);
            std::fwrite(pixels.data(), 1, pixels.size(), f);
            std::fclose(f);
            hlog("gl", "screenshot of frame %d saved to %s", frames, path.c_str());
        }
    }
    return true;
}
} // namespace gles

// ---------------- GL 1.1 core ----------------
HLE(glActiveTexture) { GLF(void, glActiveTexture, GLenum)(GLenum(arg(0))); }
HLE(glBindTexture) { glBindTexture(GLenum(arg(0)), GLuint(arg(1))); }
HLE(glBlendFunc) { glBlendFunc(GLenum(arg(0)), GLenum(arg(1))); }
HLE(glBlendFuncSeparate) { GLF(void, glBlendFuncSeparate, GLenum, GLenum, GLenum, GLenum)(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2)), GLenum(arg(3))); }
HLE(glBlendEquation) { GLF(void, glBlendEquation, GLenum)(GLenum(arg(0))); }
HLE(glClear) { glClear(GLbitfield(arg(0))); }
HLE(glClearColor) { glClearColor(F(0), F(1), F(2), F(3)); }
HLE(glClearDepthf) { glClearDepth(F(0)); }
HLE(glClearStencil) { glClearStencil(GLint(arg(0))); }
HLE(glColorMask) { glColorMask(GLboolean(arg(0)), GLboolean(arg(1)), GLboolean(arg(2)), GLboolean(arg(3))); }
HLE(glCullFace) { glCullFace(GLenum(arg(0))); }
HLE(glDepthFunc) { glDepthFunc(GLenum(arg(0))); }
HLE(glDepthMask) { glDepthMask(GLboolean(arg(0))); }
HLE(glDisable) { glDisable(GLenum(arg(0))); }
HLE(glEnable) { glEnable(GLenum(arg(0))); }
HLE(glFinish) { glFinish(); }
HLE(glFlush) { glFlush(); }
HLE(glFrontFace) { glFrontFace(GLenum(arg(0))); }
HLE(glGetError) { ret(glGetError()); }
HLE(glHint) { if (arg(0) != 0x8B8B /*FRAGMENT_SHADER_DERIVATIVE_HINT*/) glHint(GLenum(arg(0)), GLenum(arg(1))); }
HLE(glLineWidth) { glLineWidth(F(0)); }
HLE(glPixelStorei) { glPixelStorei(GLenum(arg(0)), GLint(arg(1))); }
HLE(glScissor) {
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "glScissor(%d, %d, %d, %d)", int(arg(0)), int(arg(1)), int(arg(2)), int(arg(3))); glScissor(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3))); }
HLE(glViewport) {
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "glViewport(%d, %d, %d, %d)", int(arg(0)), int(arg(1)), int(arg(2)), int(arg(3)));
    glViewport(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3)));
}
HLE(glStencilFunc) { glStencilFunc(GLenum(arg(0)), GLint(arg(1)), GLuint(arg(2))); }
HLE(glStencilOp) { glStencilOp(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2))); }
HLE(glStencilMask) { glStencilMask(GLuint(arg(0))); }
HLE(glGenTextures) { gles::ensureContext(); glGenTextures(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glDeleteTextures) { glDeleteTextures(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
HLE(glTexParameteri) { glTexParameteri(GLenum(arg(0)), GLenum(arg(1)), GLint(arg(2))); }
HLE(glTexParameterf) { glTexParameterf(GLenum(arg(0)), GLenum(arg(1)), F(0)); }
HLE(glTexImage2D) {
    // (target, level, internalformat, width, height, border, format, type, pixels)
    gles::ensureContext();
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "glTexImage2D %llux%llu fmt 0x%llx pixels %s", (unsigned long long)arg(3), (unsigned long long)arg(4), (unsigned long long)arg(6), mem::r64(cpu::sp()) ? "yes" : "null");
    GLenum type = GLenum(arg(7));
    if (type == 0x8D61) type = 0x140B; // HALF_FLOAT_OES
    GLint internal = GLint(arg(2));
    glTexImage2D(GLenum(arg(0)), GLint(arg(1)), internal, GLsizei(arg(3)), GLsizei(arg(4)), GLint(arg(5)), GLenum(arg(6)), type,
                 P(mem::r64(cpu::sp())));
    checkError("glTexImage2D");
}
HLE(glTexSubImage2D) {
    // (target, level, xoffset, yoffset, width, height, format, type, pixels)
    gles::ensureContext();
    glTexSubImage2D(GLenum(arg(0)), GLint(arg(1)), GLint(arg(2)), GLint(arg(3)), GLsizei(arg(4)), GLsizei(arg(5)), GLenum(arg(6)),
                    GLenum(arg(7)), P(mem::r64(cpu::sp())));
    checkError("glTexSubImage2D");
}
HLE(glCompressedTexImage2D) {
    // (target, level, internalformat, width, height, border, imageSize, data)
    hlog("gl", "glCompressedTexImage2D format 0x%llx %llux%llu (not supported on desktop: PVRTC/ETC need decoding)",
         (unsigned long long)arg(2), (unsigned long long)arg(3), (unsigned long long)arg(4));
}
HLE(glReadPixels) {
    glReadPixels(GLint(arg(0)), GLint(arg(1)), GLsizei(arg(2)), GLsizei(arg(3)), GLenum(arg(4)), GLenum(arg(5)), PW(arg(6)));
}
HLE(glGetIntegerv) {
    const GLenum pname = GLenum(arg(0));
    GLint values[16] = {};
    if (pname == 0x8DFB /*MAX_VERTEX_UNIFORM_VECTORS*/) values[0] = 128;
    else if (pname == 0x8DFD /*MAX_FRAGMENT_UNIFORM_VECTORS*/) values[0] = 64;
    else if (pname == 0x8DFC /*MAX_VARYING_VECTORS*/) values[0] = 8;
    else if (pname == 0x86A2 /*NUM_COMPRESSED_TEXTURE_FORMATS*/) values[0] = 0;
    else if (pname == 0x8B9A /*IMPLEMENTATION_COLOR_READ_TYPE*/) values[0] = GL_UNSIGNED_BYTE;
    else if (pname == 0x8B9B /*IMPLEMENTATION_COLOR_READ_FORMAT*/) values[0] = GL_RGBA;
    else if (pname == GL_MAX_TEXTURE_SIZE) values[0] = 4096;
    else glGetIntegerv(pname, values);
    mem::w32(arg(1), std::uint32_t(values[0]));
    if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) for (int i = 1; i < 4; ++i) mem::w32(arg(1) + 4 * i, std::uint32_t(values[i]));
}
HLE(glGetFloatv) {
    GLfloat values[16] = {};
    glGetFloatv(GLenum(arg(0)), values);
    std::memcpy(PW(arg(1)), values, sizeof(GLfloat));
}
HLE(glGetBooleanv) {
    GLboolean v[16] = {};
    glGetBooleanv(GLenum(arg(0)), v);
    mem::write<std::uint8_t>(arg(1), v[0]);
}
HLE(glIsEnabled) { ret(glIsEnabled(GLenum(arg(0)))); }
HLE(glGetString) {
    static std::uint64_t vendor = mem::hleString("Apple Inc."), renderer = mem::hleString("Apple A8 GPU"),
                         version = mem::hleString("OpenGL ES 2.0 Apple A8 GPU - 77.14"),
                         glsl = mem::hleString("OpenGL ES GLSL ES 1.00"),
                         extensions = mem::hleString(
                             "GL_OES_depth24 GL_OES_element_index_uint GL_OES_fbo_render_mipmap GL_OES_mapbuffer GL_OES_packed_depth_stencil "
                             "GL_OES_rgb8_rgba8 GL_OES_standard_derivatives GL_OES_texture_float GL_OES_texture_half_float "
                             "GL_OES_vertex_array_object GL_EXT_blend_minmax GL_EXT_debug_label GL_EXT_discard_framebuffer "
                             "GL_EXT_read_format_bgra GL_EXT_texture_filter_anisotropic GL_APPLE_framebuffer_multisample "
                             "GL_APPLE_rgb_422 GL_APPLE_texture_format_BGRA8888 GL_APPLE_texture_max_level");
    switch (arg(0)) {
    case GL_VENDOR: ret(vendor); break;
    case GL_RENDERER: ret(renderer); break;
    case GL_VERSION: ret(version); break;
    case 0x8B8C: ret(glsl); break;
    case GL_EXTENSIONS: ret(extensions); break;
    default: ret(0);
    }
}
HLE(glDrawArrays) { glDrawArrays(GLenum(arg(0)), GLint(arg(1)), GLsizei(arg(2))); checkError("glDrawArrays"); }
HLE(glDrawElements) {
    const bool vbo = binding(ELEMENT_ARRAY_BUFFER_BINDING) != 0;
    glDrawElements(GLenum(arg(0)), GLsizei(arg(1)), GLenum(arg(2)), vbo ? reinterpret_cast<const void *>(arg(3)) : P(arg(3)));
    checkError("glDrawElements");
}

// ---------------- Buffers ----------------
HLE(glGenBuffers) { gles::ensureContext(); GLF(void, glGenBuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glDeleteBuffers) { GLF(void, glDeleteBuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
HLE(glBindBuffer) { GLF(void, glBindBuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
HLE(glBufferData) { GLF(void, glBufferData, GLenum, std::ptrdiff_t, const void *, GLenum)(GLenum(arg(0)), std::ptrdiff_t(arg(1)), P(arg(2)), GLenum(arg(3))); }
HLE(glBufferSubData) {
    GLF(void, glBufferSubData, GLenum, std::ptrdiff_t, std::ptrdiff_t, const void *)(GLenum(arg(0)), std::ptrdiff_t(arg(1)), std::ptrdiff_t(arg(2)), P(arg(3)));
}
HLE(glVertexAttribPointer) {
    // (index, size, type, normalized, stride, pointer): an offset when an array buffer is bound, else a guest address.
    const bool vbo = binding(ARRAY_BUFFER_BINDING) != 0;
    GLenum type = GLenum(arg(2));
    if (type == 0x8D61) type = 0x140B;
    GLF(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)
    (GLuint(arg(0)), GLint(arg(1)), type, GLboolean(arg(3)), GLsizei(arg(4)), vbo ? reinterpret_cast<const void *>(arg(5)) : P(arg(5)));
}
HLE(glEnableVertexAttribArray) { GLF(void, glEnableVertexAttribArray, GLuint)(GLuint(arg(0))); }
HLE(glDisableVertexAttribArray) { GLF(void, glDisableVertexAttribArray, GLuint)(GLuint(arg(0))); }
HLE(glVertexAttrib4f) { GLF(void, glVertexAttrib4f, GLuint, float, float, float, float)(GLuint(arg(0)), F(0), F(1), F(2), F(3)); }
HLE(glGenVertexArraysOES) { GLF(void, glGenVertexArrays, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
HLE(glBindVertexArrayOES) { GLF(void, glBindVertexArray, GLuint)(GLuint(arg(0))); }
HLE(glDeleteVertexArraysOES) { GLF(void, glDeleteVertexArrays, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }

// ---------------- Shaders & programs ----------------
HLE(glCreateShader) { gles::ensureContext(); ret(GLF(GLuint, glCreateShader, GLenum)(GLenum(arg(0)))); }
HLE(glDeleteShader) { GLF(void, glDeleteShader, GLuint)(GLuint(arg(0))); }
HLE(glShaderSource) {
    // (shader, count, const char **strings, const GLint *lengths)
    std::string source;
    for (std::uint64_t i = 0; i < arg(1); ++i) {
        const auto s = mem::r64(arg(2) + 8 * i);
        if (arg(3) && std::int32_t(mem::r32(arg(3) + 4 * i)) >= 0)
            source.append(reinterpret_cast<const char *>(mem::base() + s), mem::r32(arg(3) + 4 * i));
        else source += mem::cstring(s);
    }
    const auto translated = translateShader(source);
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "shader %u source:%c%s", unsigned(arg(0)), 10, source.c_str());
    const char *text = translated.c_str();
    GLF(void, glShaderSource, GLuint, GLsizei, const char *const *, const GLint *)(GLuint(arg(0)), 1, &text, nullptr);
}
HLE(glCompileShader) {
    GLF(void, glCompileShader, GLuint)(GLuint(arg(0)));
    GLint ok = 0;
    GLF(void, glGetShaderiv, GLuint, GLenum, GLint *)(GLuint(arg(0)), 0x8B81, &ok);
    if (!ok) {
        char log[2048] = {};
        GLF(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, char *)(GLuint(arg(0)), sizeof log, nullptr, log);
        hlog("gl", "shader %u failed to compile: %s", unsigned(arg(0)), log);
    }
}
HLE(glGetShaderiv) {
    GLint v = 0;
    GLF(void, glGetShaderiv, GLuint, GLenum, GLint *)(GLuint(arg(0)), GLenum(arg(1)), &v);
    mem::w32(arg(2), std::uint32_t(v));
}
HLE(glGetShaderInfoLog) {
    GLsizei n = 0;
    GLF(void, glGetShaderInfoLog, GLuint, GLsizei, GLsizei *, char *)(GLuint(arg(0)), GLsizei(arg(1)), &n, static_cast<char *>(PW(arg(3))));
    if (arg(2)) mem::w32(arg(2), std::uint32_t(n));
}
HLE(glCreateProgram) { gles::ensureContext(); ret(GLF(GLuint, glCreateProgram, void)()); }
HLE(glDeleteProgram) { GLF(void, glDeleteProgram, GLuint)(GLuint(arg(0))); }
HLE(glAttachShader) { GLF(void, glAttachShader, GLuint, GLuint)(GLuint(arg(0)), GLuint(arg(1))); }
HLE(glDetachShader) { GLF(void, glDetachShader, GLuint, GLuint)(GLuint(arg(0)), GLuint(arg(1))); }
HLE(glBindAttribLocation) { GLF(void, glBindAttribLocation, GLuint, GLuint, const char *)(GLuint(arg(0)), GLuint(arg(1)), static_cast<const char *>(P(arg(2)))); }
HLE(glLinkProgram) {
    GLF(void, glLinkProgram, GLuint)(GLuint(arg(0)));
    GLint ok = 0;
    GLF(void, glGetProgramiv, GLuint, GLenum, GLint *)(GLuint(arg(0)), 0x8B82, &ok);
    if (!ok) {
        char log[2048] = {};
        GLF(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei *, char *)(GLuint(arg(0)), sizeof log, nullptr, log);
        hlog("gl", "program %u failed to link: %s", unsigned(arg(0)), log);
    }
}
HLE(glGetProgramiv) {
    GLint v = 0;
    GLF(void, glGetProgramiv, GLuint, GLenum, GLint *)(GLuint(arg(0)), GLenum(arg(1)), &v);
    mem::w32(arg(2), std::uint32_t(v));
}
HLE(glGetProgramInfoLog) {
    GLsizei n = 0;
    GLF(void, glGetProgramInfoLog, GLuint, GLsizei, GLsizei *, char *)(GLuint(arg(0)), GLsizei(arg(1)), &n, static_cast<char *>(PW(arg(3))));
    if (arg(2)) mem::w32(arg(2), std::uint32_t(n));
}
HLE(glUseProgram) { GLF(void, glUseProgram, GLuint)(GLuint(arg(0))); }
HLE(glValidateProgram) { GLF(void, glValidateProgram, GLuint)(GLuint(arg(0))); }
HLE(glGetUniformLocation) { ret(std::uint64_t(std::int64_t(GLF(GLint, glGetUniformLocation, GLuint, const char *)(GLuint(arg(0)), static_cast<const char *>(P(arg(1))))))); }
HLE(glGetAttribLocation) { ret(std::uint64_t(std::int64_t(GLF(GLint, glGetAttribLocation, GLuint, const char *)(GLuint(arg(0)), static_cast<const char *>(P(arg(1))))))); }
HLE(glUniform1i) { GLF(void, glUniform1i, GLint, GLint)(GLint(arg(0)), GLint(arg(1))); }
HLE(glUniform1f) { GLF(void, glUniform1f, GLint, float)(GLint(arg(0)), F(0)); }
HLE(glUniform2f) { GLF(void, glUniform2f, GLint, float, float)(GLint(arg(0)), F(0), F(1)); }
HLE(glUniform3f) { GLF(void, glUniform3f, GLint, float, float, float)(GLint(arg(0)), F(0), F(1), F(2)); }
HLE(glUniform4f) {
    static int logged = 0;
    if (std::getenv("HLE_GLTRACE") && logged++ < 10) hlog("gl", "glUniform4f loc %d: %g %g %g %g", int(arg(0)), F(0), F(1), F(2), F(3)); GLF(void, glUniform4f, GLint, float, float, float, float)(GLint(arg(0)), F(0), F(1), F(2), F(3)); }
HLE(glUniform1fv) { GLF(void, glUniform1fv, GLint, GLsizei, const float *)(GLint(arg(0)), GLsizei(arg(1)), static_cast<const float *>(P(arg(2)))); }
HLE(glUniform2fv) { GLF(void, glUniform2fv, GLint, GLsizei, const float *)(GLint(arg(0)), GLsizei(arg(1)), static_cast<const float *>(P(arg(2)))); }
HLE(glUniform4fv) { GLF(void, glUniform4fv, GLint, GLsizei, const float *)(GLint(arg(0)), GLsizei(arg(1)), static_cast<const float *>(P(arg(2)))); }
HLE(glUniformMatrix4fv) {
    static int logged = 0;
    if (std::getenv("HLE_GLTRACE") && logged++ < 10) {
        const float *m = static_cast<const float *>(P(arg(3)));
        hlog("gl", "glUniformMatrix4fv loc %d: [%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g]", int(arg(0)), m[0], m[1], m[2], m[3], m[4], m[5],
             m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }
    GLF(void, glUniformMatrix4fv, GLint, GLsizei, GLboolean, const float *)(GLint(arg(0)), GLsizei(arg(1)), GLboolean(arg(2)), static_cast<const float *>(P(arg(3))));
}

// ---------------- Framebuffers (core and OES names) ----------------
static void genFramebuffers() { gles::ensureContext(); GLF(void, glGenFramebuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
static void bindFramebuffer() {
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "glBindFramebuffer(0x%x, %u)", unsigned(arg(0)), unsigned(arg(1))); GLF(void, glBindFramebuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
static void deleteFramebuffers() { GLF(void, glDeleteFramebuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
static void genRenderbuffers() { gles::ensureContext(); GLF(void, glGenRenderbuffers, GLsizei, GLuint *)(GLsizei(arg(0)), static_cast<GLuint *>(PW(arg(1)))); }
static void bindRenderbuffer() { GLF(void, glBindRenderbuffer, GLenum, GLuint)(GLenum(arg(0)), GLuint(arg(1))); }
static void deleteRenderbuffers() { GLF(void, glDeleteRenderbuffers, GLsizei, const GLuint *)(GLsizei(arg(0)), static_cast<const GLuint *>(P(arg(1)))); }
static void renderbufferStorage() {
    GLenum format = GLenum(arg(1));
    if (format == 0x8D62 /*RGB565*/) format = RGBA8;
    GLF(void, glRenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)(GLenum(arg(0)), format, GLsizei(arg(2)), GLsizei(arg(3)));
}
static void framebufferRenderbuffer() {
    GLF(void, glFramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2)), GLuint(arg(3)));
}
static void checkFramebufferStatus() { ret(GLF(GLenum, glCheckFramebufferStatus, GLenum)(GLenum(arg(0)))); }
static hle::Registrar r1("_glGenFramebuffers", genFramebuffers), r2("_glGenFramebuffersOES", genFramebuffers);
static hle::Registrar r3("_glBindFramebuffer", bindFramebuffer), r4("_glBindFramebufferOES", bindFramebuffer);
static hle::Registrar r5("_glDeleteFramebuffers", deleteFramebuffers), r6("_glDeleteFramebuffersOES", deleteFramebuffers);
static hle::Registrar r7("_glGenRenderbuffers", genRenderbuffers), r8("_glGenRenderbuffersOES", genRenderbuffers);
static hle::Registrar r9("_glBindRenderbuffer", bindRenderbuffer), r10("_glBindRenderbufferOES", bindRenderbuffer);
static hle::Registrar r11("_glDeleteRenderbuffers", deleteRenderbuffers), r12("_glDeleteRenderbuffersOES", deleteRenderbuffers);
static hle::Registrar r13("_glRenderbufferStorage", renderbufferStorage), r14("_glRenderbufferStorageOES", renderbufferStorage);
static hle::Registrar r15("_glFramebufferRenderbuffer", framebufferRenderbuffer), r16("_glFramebufferRenderbufferOES", framebufferRenderbuffer);
static hle::Registrar r17("_glCheckFramebufferStatus", checkFramebufferStatus), r18("_glCheckFramebufferStatusOES", checkFramebufferStatus);
HLE(glFramebufferTexture2D) {
    if (std::getenv("HLE_GLTRACE")) hlog("gl", "glFramebufferTexture2D(tex %u)", unsigned(arg(3)));
    GLF(void, glFramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)(GLenum(arg(0)), GLenum(arg(1)), GLenum(arg(2)), GLuint(arg(3)), GLint(arg(4)));
}
HLE(glGenerateMipmap) { GLF(void, glGenerateMipmap, GLenum)(GLenum(arg(0))); }
HLE(glRenderbufferStorageMultisampleAPPLE) {
    GLF(void, glRenderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei)(GLenum(arg(0)), GLsizei(arg(1)), GLenum(arg(2)), GLsizei(arg(3)), GLsizei(arg(4)));
}
HLE(glResolveMultisampleFramebufferAPPLE) {
    GLF(void, glBlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
    (0, 0, platform::kPixelsWidth, platform::kPixelsHeight, 0, 0, platform::kPixelsWidth, platform::kPixelsHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
HLE(glDiscardFramebufferEXT) {}
