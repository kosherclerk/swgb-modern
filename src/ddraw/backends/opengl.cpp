// OpenGL 3.3 core backend (WGL). Functions are loaded at runtime.
#include "../renderer.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>
#include <string>

namespace swgbm {
namespace {

#define GLFUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM4FPROC, glUniform4f) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) \
    X(PFNGLDELETESHADERPROC, glDeleteShader) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)

#define DECL(T, N) T N = nullptr;
GLFUNCS(DECL)
#undef DECL

class GLRenderer : public Renderer {
public:
    const char* Name() const override { return "OpenGL 3.3"; }
    ~GLRenderer() override {
        if (rc_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(rc_);
        }
        if (dc_) ReleaseDC(hwnd_, dc_);
    }

    bool Init(HWND hwnd) override {
        hwnd_ = hwnd;
        dc_ = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1};
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        int pf = ChoosePixelFormat(dc_, &pfd);
        if (!pf) return false;
        if (GetPixelFormat(dc_) == 0 && !SetPixelFormat(dc_, pf, &pfd)) { LOG("GL: SetPixelFormat failed"); return false; }
        HGLRC tmp = wglCreateContext(dc_);
        if (!tmp || !wglMakeCurrent(dc_, tmp)) { LOG("GL: legacy context failed"); return false; }
        auto createAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        if (createAttribs) {
            int attrs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                           WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
            rc_ = createAttribs(dc_, nullptr, attrs);
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(tmp);
        if (!rc_ || !wglMakeCurrent(dc_, rc_)) { LOG("GL: 3.3 core context unavailable"); return false; }
        swapInterval_ = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
#define LOAD(T, N) N = (T)wglGetProcAddress(#N); if (!N) { LOG("GL: missing " #N); return false; }
        GLFUNCS(LOAD)
#undef LOAD
        LOG("GL: %s / %s", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));

        progPal_ = Program(g_glslPaletteFrag);
        progScale_ = Program(g_glslScaleFrag);
        progOverlay_ = Program(g_glslOverlayFrag);
        if (!progPal_ || !progScale_ || !progOverlay_) return false;
        glUseProgram(progOverlay_);
        glUniform1i(glGetUniformLocation(progOverlay_, "OverlayTex"), 0);
        glGenVertexArrays(1, &vao_);
        glGenTextures(1, &palTex_);
        glBindTexture(GL_TEXTURE_2D, palTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        SetParams(GL_NEAREST);
        glGenFramebuffers(1, &fbo_);
        uPalH_ = glGetUniformLocation(progPal_, "frameH");
        glUseProgram(progPal_);
        glUniform1i(glGetUniformLocation(progPal_, "IndexTex"), 0);
        glUniform1i(glGetUniformLocation(progPal_, "PaletteTex"), 1);
        glUseProgram(progScale_);
        glUniform1i(glGetUniformLocation(progScale_, "FrameTex"), 0);
        uSrc_ = glGetUniformLocation(progScale_, "srcSize");
        uDst_ = glGetUniformLocation(progScale_, "dstSize");
        uFilter_ = glGetUniformLocation(progScale_, "filterMode");
        uUv_ = glGetUniformLocation(progScale_, "uvRect");
        uSharp_ = glGetUniformLocation(progScale_, "sharpness");
        return glGetError() == GL_NO_ERROR;
    }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (cw <= 0 || ch <= 0) return true;
        bool fresh = false;
        if (f.width != fw_ || f.height != fh_) { CreateFrameResources(f.width, f.height); fresh = true; }
        if (swapInterval_ && opt.vsync != lastVsync_) { swapInterval_(opt.vsync ? 1 : 0); lastVsync_ = opt.vsync; }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, idxTex_);
        if (f.dirtyY1 > f.dirtyY0)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, f.dirtyY0, f.width, f.dirtyY1 - f.dirtyY0, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                            f.pixels.data() + (size_t)f.dirtyY0 * f.width);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, palTex_);
        if (f.paletteChanged) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_BGRA, GL_UNSIGNED_BYTE, f.palette);
        glBindVertexArray(vao_);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        // pass 1 (palette resolve), only when the indices or the palette
        // changed; otherwise the frame texture is still current (zoom
        // animation, resize, overlay changes re-present the same frame)
        if (fresh || f.Dirty()) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
            glViewport(0, 0, f.width, f.height);
            glUseProgram(progPal_);
            glUniform1i(uPalH_, f.height);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        // pass 2; clear only when the image leaves bars
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (vp.x > 0 || vp.y > 0 || vp.w < cw || vp.h < ch || (overlay_ && overlay_->visible)) {
            glViewport(0, 0, cw, ch);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, frameTex_);
        glUseProgram(progScale_);
        glUniform4f(uSrc_, (float)f.width, (float)f.height, 1.0f / f.width, 1.0f / f.height);
        glUniform1f(uSharp_, opt.sharpness);
        if (!(overlay_ && overlay_->visible && overlay_->hideFrame))
            for (const Region& rg : RegionsFor(vp, opt)) {
                glViewport(rg.x, ch - rg.y - rg.h, rg.w, rg.h);
                glUniform4f(uDst_, (float)rg.w, (float)rg.h, 1.0f / rg.w, 1.0f / rg.h);
                glUniform1i(uFilter_, (int)rg.filter);
                glUniform4f(uUv_, rg.u0, rg.v0, rg.u1 - rg.u0, rg.v1 - rg.v0);
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        DrawOverlay(f, vp, ch);
        SwapBuffers(dc_);
        // glGetError can make the driver synchronise: check once a second
        if (++frameNo_ % 60 == 0) { GLenum e = glGetError(); if (e != GL_NO_ERROR) LOG("GL error %04x", e); }
        return true;
    }

private:
    void DrawOverlay(const Frame& f, const Viewport& vp, int ch) {
        const Overlay* o = overlay_;
        if (!o || !o->visible || o->width <= 0 || o->height <= 0) return;
        glActiveTexture(GL_TEXTURE0);
        bool full = false;
        if (!ovTex_ || o->width != ovW_ || o->height != ovH_) {
            if (!ovTex_) glGenTextures(1, &ovTex_);
            glBindTexture(GL_TEXTURE_2D, ovTex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, o->width, o->height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            SetParams(GL_LINEAR);
            ovW_ = o->width; ovH_ = o->height; full = true;
        }
        glBindTexture(GL_TEXTURE_2D, ovTex_);
        int y0 = full ? 0 : o->dirtyY0, y1 = full ? o->height : o->dirtyY1;
        if (y1 > y0)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, o->width, y1 - y0, GL_BGRA, GL_UNSIGNED_BYTE, o->pixels.data() + (size_t)y0 * o->width);
        float x, y, w, h;
        OverlayRect(*o, f, vp, x, y, w, h);
        glViewport((GLint)x, (GLint)(ch - y - h), (GLsizei)(w + 0.5f), (GLsizei)(h + 0.5f));
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(progOverlay_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
    }
    static void SetParams(GLint filt) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    void CreateFrameResources(int w, int h) {
        if (idxTex_) { glDeleteTextures(1, &idxTex_); glDeleteTextures(1, &frameTex_); }
        glGenTextures(1, &idxTex_);
        glBindTexture(GL_TEXTURE_2D, idxTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
        SetParams(GL_NEAREST);
        glGenTextures(1, &frameTex_);
        glBindTexture(GL_TEXTURE_2D, frameTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        SetParams(GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameTex_, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fw_ = w; fh_ = h;
    }
    GLuint Shader(GLenum type, const char* body) {
        std::string src = "#version 330 core\n";
        src += body;
        const char* s = src.c_str();
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &s, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof(log), nullptr, log); LOG("GLSL compile error: %s", log); return 0; }
        return sh;
    }
    GLuint Program(const char* frag) {
        GLuint vs = Shader(GL_VERTEX_SHADER, g_glslVertex), fs = Shader(GL_FRAGMENT_SHADER, frag);
        if (!vs || !fs) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof(log), nullptr, log); LOG("GLSL link error: %s", log); return 0; }
        return p;
    }

    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HGLRC rc_ = nullptr;
    PFNWGLSWAPINTERVALEXTPROC swapInterval_ = nullptr;
    int lastVsync_ = -1;
    GLuint progOverlay_ = 0, ovTex_ = 0;
    int ovW_ = 0, ovH_ = 0;
    GLuint progPal_ = 0, progScale_ = 0, vao_ = 0, fbo_ = 0, idxTex_ = 0, palTex_ = 0, frameTex_ = 0;
    unsigned frameNo_ = 0;
    GLint uPalH_ = -1, uSrc_ = -1, uDst_ = -1, uFilter_ = -1, uSharp_ = -1, uUv_ = -1;
    int fw_ = 0, fh_ = 0;
};

} // namespace

std::unique_ptr<Renderer> CreateOpenGLRenderer() { return std::make_unique<GLRenderer>(); }

} // namespace swgbm
