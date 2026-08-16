#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <cstdint>
namespace rp {

// Minimal GL types (avoid a GL header / loader dep).
using GLenum = unsigned int; using GLbitfield = unsigned int; using GLuint = unsigned int;
using GLint = int; using GLsizei = int; using GLfloat = float; using GLchar = char;
using GLboolean = unsigned char; using GLintptr = intptr_t; using GLsizeiptr = intptr_t;

// The ~20 entrypoints used by GLCompositor + GLBackend. (GL 1.1 ones come from opengl32; the rest from
// wglGetProcAddress.) Extend here if a later task needs more.
struct GLFns {
    const unsigned char* (WINAPI *GetString)(GLenum);
    void (WINAPI *GetIntegerv)(GLenum, GLint*);
    void (WINAPI *Viewport)(GLint,GLint,GLsizei,GLsizei);
    void (WINAPI *Clear)(GLbitfield); void (WINAPI *ClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
    void (WINAPI *GenTextures)(GLsizei,GLuint*); void (WINAPI *DeleteTextures)(GLsizei,const GLuint*);
    void (WINAPI *BindTexture)(GLenum,GLuint);
    void (WINAPI *TexParameteri)(GLenum,GLenum,GLint);
    void (WINAPI *TexImage2D)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
    void (WINAPI *TexSubImage2D)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*);
    void (WINAPI *PixelStorei)(GLenum,GLint);
    void (WINAPI *ReadPixels)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
    void (WINAPI *DrawArrays)(GLenum,GLint,GLsizei);
    // FBO + VAO + shaders (core-profile; all via wglGetProcAddress).
    void (WINAPI *GenFramebuffers)(GLsizei,GLuint*); void (WINAPI *BindFramebuffer)(GLenum,GLuint);
    void (WINAPI *FramebufferTexture2D)(GLenum,GLenum,GLenum,GLuint,GLint);
    GLenum (WINAPI *CheckFramebufferStatus)(GLenum);
    void (WINAPI *GenVertexArrays)(GLsizei,GLuint*); void (WINAPI *BindVertexArray)(GLuint);
    GLuint (WINAPI *CreateShader)(GLenum); void (WINAPI *ShaderSource)(GLuint,GLsizei,const GLchar* const*,const GLint*);
    void (WINAPI *CompileShader)(GLuint); void (WINAPI *GetShaderiv)(GLuint,GLenum,GLint*);
    void (WINAPI *GetShaderInfoLog)(GLuint,GLsizei,GLsizei*,GLchar*);
    GLuint (WINAPI *CreateProgram)(void); void (WINAPI *AttachShader)(GLuint,GLuint);
    void (WINAPI *LinkProgram)(GLuint); void (WINAPI *GetProgramiv)(GLuint,GLenum,GLint*);
    void (WINAPI *GetProgramInfoLog)(GLuint,GLsizei,GLsizei*,GLchar*);
    void (WINAPI *UseProgram)(GLuint); void (WINAPI *DeleteShader)(GLuint);
    GLint (WINAPI *GetUniformLocation)(GLuint,const GLchar*); void (WINAPI *Uniform1i)(GLint,GLint);
    void (WINAPI *Uniform2f)(GLint,GLfloat,GLfloat);
    void (WINAPI *GetTexLevelParameteriv)(GLenum,GLint,GLenum,GLint*);
    void (WINAPI *ActiveTexture)(GLenum);
    void (WINAPI *GenRenderbuffers)(GLsizei,GLuint*);
    void (WINAPI *BindRenderbuffer)(GLenum,GLuint);
    void (WINAPI *RenderbufferStorage)(GLenum,GLenum,GLsizei,GLsizei);
    void (WINAPI *FramebufferRenderbuffer)(GLenum,GLenum,GLenum,GLuint);
    void (WINAPI *Enable)(GLenum);
    void (WINAPI *Disable)(GLenum);
    void (WINAPI *Scissor)(GLint,GLint,GLsizei,GLsizei);
};

class GLContext {
public:
    GLContext() = default;
    ~GLContext();
    GLContext(const GLContext&) = delete; GLContext& operator=(const GLContext&) = delete;

    // native_window == nullptr => headless (hidden window). Creates a GL 3.3-core context + loads GLFns.
    // share_context (an HGLRC) => the new context shares objects (textures, etc.) with it (B2 zero-copy).
    bool initialize(void* native_window, uint32_t w, uint32_t h, std::string& err, int major = 3, int minor = 3,
                    void* share_context = nullptr);
    bool make_current();
    void present();          // SwapBuffers (windowed) / glFlush (headless)
    const GLFns& gl() const { return fns_; }
    void* hglrc() const { return hglrc_; }   // the WGL context handle (to share with)
    bool windowed() const { return owns_window_ == false; }  // we own a hidden window only when headless
    uint32_t width() const { return w_; } uint32_t height() const { return h_; }
    // Live client-area size of the presenting window (windowed only), for scaling the
    // windowed viewport to fill the window instead of the core's native render size.
    void client_size(uint32_t& w, uint32_t& h) const;

    // Can a 3.3-core context be created on this machine? (headless probe; used to gate tests)
    static bool probe();

private:
    void destroy();
    HWND  hwnd_ = nullptr; bool owns_window_ = false;
    HDC   hdc_ = nullptr; HGLRC hglrc_ = nullptr;
    GLFns fns_{}; uint32_t w_ = 0, h_ = 0;
};
} // namespace rp
