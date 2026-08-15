# RetroPark OpenGL Host Backend — Implementation Plan (Subsystem A)

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add a third `IRenderBackend` (`RP_GFX_OPENGL`) that composites driven-core CPU frames via OpenGL
3.3-core, headless (`glReadPixels`) and windowed (WGL `SwapBuffers`), proven with `refcore_driven` + real NES,
exposed in EverythingBox as a user-selectable driven backend, and deployed.

**Architecture:** New `src/render/gl/` mirroring `src/render/vulkan/`: `GLContext` (WGL seam + entrypoint
loader + probe), `GLCompositor` (+`GLShaders.h`, textured fullscreen quad), `GLBackend` (`IRenderBackend`).
Wired into `make_backend`. Driven cores only; presenting-frame path returns `RP_ERR_UNSUPPORTED`.

**Tech Stack:** C++17, hand-rolled WGL (no GL loader dep), system `opengl32`/`gdi32`, doctest, CMake/MSBuild.

**Reference spec:** `docs/superpowers/specs/2026-08-15-retropark-opengl-host-backend-design.md`.

## Global Constraints

- **No new third-party dependency.** Load the ~20 GL entrypoints by hand via `wglGetProcAddress`; link only
  system `opengl32.lib` + `gdi32.lib`. No glad/GLEW.
- **`RP_GFX_OPENGL = 3` is additive — do NOT bump `RETROPARK_ABI_VERSION`** (currently 7). No struct changes;
  driven cores report `RP_GFX_NONE`; `RP_GFX_OPENGL` is only ever passed to `rp_runtime_create`.
- **Driven cores only.** `composite_and_present` → `RP_ERR_UNSUPPORTED`; `allocate_surfaces` → `RP_OK` with
  `count` inert descs (zeroed `shared_handle`) so the driven-core resize path (`Runtime::resize` →
  `rebuild_surfaces` → `allocate_surfaces`) keeps working, exactly as D3D11/Vulkan do.
- **Windows/WGL only** this slice; keep all Win32/WGL inside `GLContext` so a GLX/EGL variant slots in later.
- GL is bottom-left origin; `out_rgba` is top-left — **Y-flip on `glReadPixels`**.
- No overlay (removed brand-wide). The compositor draws only the core frame.
- No AI attribution in commits/PRs. At slice end merge to `main` + push (RetroPark convention).

---

### Task 1: `RP_GFX_OPENGL` enum + `GLContext` (WGL context, entrypoint loader, probe)

**Files:**
- Modify: `include/retropark/retropark_abi.h` (add the enum value)
- Create: `src/render/gl/GLContext.h`, `src/render/gl/GLContext.cpp`
- Modify: `CMakeLists.txt` (add `src/render/gl/*.cpp` to the `retropark` lib target sources + link
  `opengl32`, `gdi32`)
- Test: `tests/test_gl_context.cpp`

**Interfaces:**
- Produces: `rp::GLContext` — RAII WGL context; `static bool GLContext::probe()` (a 3.3-core context can be
  created here); a loaded `GLFns` function table (member `gl()`), `make_current()`, `present()` (SwapBuffers
  when windowed / glFlush when headless), `int width()/height()`.
- Consumes: `include/retropark/retropark_abi.h` (`RP_GFX_OPENGL`, `rp_result`).

- [ ] **Step 1: Add the enum value.** In `include/retropark/retropark_abi.h`, extend `rp_graphics_api`:

```c
typedef enum rp_graphics_api {
    RP_GFX_D3D11 = 0,
    RP_GFX_VULKAN = 1,
    RP_GFX_NONE = 2,            /* driven cores: no host-managed swapchain */
    RP_GFX_OPENGL = 3          /* OpenGL host compositor (driven cores). Additive: no ABI-version bump. */
} rp_graphics_api;
```

- [ ] **Step 2: Write the failing probe test** (`tests/test_gl_context.cpp`):

```cpp
#include <doctest/doctest.h>
#include "render/gl/GLContext.h"
using namespace rp;

TEST_CASE("gl context: probe + make a 3.3-core context") {
    if (!GLContext::probe()) { WARN("no capable OpenGL 3.3 context; skipping"); return; }
    std::string err;
    GLContext ctx;
    REQUIRE(ctx.initialize(/*native_window=*/nullptr, 64, 64, err));   // headless
    REQUIRE(ctx.make_current());
    // GL_VERSION must be >= 3.3 core.
    const char* ver = (const char*)ctx.gl().GetString(0x1F02 /*GL_VERSION*/);
    REQUIRE(ver != nullptr);
    int major = 0, minor = 0;
    ctx.gl().GetIntegerv(0x821B /*GL_MAJOR_VERSION*/, &major);
    ctx.gl().GetIntegerv(0x821C /*GL_MINOR_VERSION*/, &minor);
    CHECK((major > 3 || (major == 3 && minor >= 3)));
}
```

Run: `cmake --build build --config Release --target retropark_tests` → FAIL (no `GLContext.h`).

- [ ] **Step 3: Implement `GLContext.h`.** Declare the function table (only the entrypoints the backend +
  compositor need) and the RAII context. Keep it a plain struct of `PFN` typedefs to avoid pulling a GL
  header — declare our own `typedef` aliases for `GLenum/GLuint/GLint/GLsizei/GLchar/GLfloat/GLbitfield` and
  `void*` handles.

```cpp
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
    void (WINAPI *ActiveTexture)(GLenum);
};

class GLContext {
public:
    GLContext() = default;
    ~GLContext();
    GLContext(const GLContext&) = delete; GLContext& operator=(const GLContext&) = delete;

    // native_window == nullptr => headless (hidden window). Creates a GL 3.3-core context + loads GLFns.
    bool initialize(void* native_window, uint32_t w, uint32_t h, std::string& err);
    bool make_current();
    void present();          // SwapBuffers (windowed) / glFlush (headless)
    const GLFns& gl() const { return fns_; }
    bool windowed() const { return owns_window_ == false; }  // we own a hidden window only when headless
    uint32_t width() const { return w_; } uint32_t height() const { return h_; }

    // Can a 3.3-core context be created on this machine? (headless probe; used to gate tests)
    static bool probe();

private:
    void destroy();
    HWND  hwnd_ = nullptr; bool owns_window_ = false;
    HDC   hdc_ = nullptr; HGLRC hglrc_ = nullptr;
    GLFns fns_{}; uint32_t w_ = 0, h_ = 0;
};
} // namespace rp
```

- [ ] **Step 4: Implement `GLContext.cpp`.** Standard WGL bootstrap: a throwaway dummy window/context to
  fetch `wglChoosePixelFormatARB` + `wglCreateContextAttribsARB`, then the real context (3.3 core). Then load
  `GLFns` (each via `wglGetProcAddress`, falling back to `GetProcAddress(opengl32.dll)` for the GL-1.1
  symbols: `GetString/GetIntegerv/Viewport/Clear/ClearColor/GenTextures/DeleteTextures/BindTexture/
  TexParameteri/TexImage2D/TexSubImage2D/PixelStorei/ReadPixels/DrawArrays`).

```cpp
#include "render/gl/GLContext.h"
namespace rp {

// WGL ARB constants (avoid wglext.h).
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define WGL_DRAW_TO_WINDOW_ARB 0x2001
#define WGL_SUPPORT_OPENGL_ARB 0x2010
#define WGL_DOUBLE_BUFFER_ARB  0x2011
#define WGL_PIXEL_TYPE_ARB     0x2013
#define WGL_TYPE_RGBA_ARB      0x202B
#define WGL_COLOR_BITS_ARB     0x2014
#define WGL_ALPHA_BITS_ARB     0x201B
#define WGL_DEPTH_BITS_ARB     0x2022

typedef HGLRC (WINAPI *PFNWGLCREATECTXATTRIBS)(HDC, HGLRC, const int*);
typedef BOOL  (WINAPI *PFNWGLCHOOSEPIXELFMT)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);

static HWND make_hidden_window() {
    WNDCLASSA wc{}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RPGLHiddenWnd"; RegisterClassA(&wc);   // idempotent enough; ignore re-register
    return CreateWindowExA(0, "RPGLHiddenWnd", "", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

// Bootstrap: dummy ctx -> load the two ARB fns.
static bool load_wgl_arb(PFNWGLCREATECTXATTRIBS& createAttribs, PFNWGLCHOOSEPIXELFMT& choosePf) {
    HWND dw = make_hidden_window(); if (!dw) return false;
    HDC ddc = GetDC(dw);
    PIXELFORMATDESCRIPTOR pfd{}; pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cAlphaBits = 8; pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(ddc, &pfd); SetPixelFormat(ddc, pf, &pfd);
    HGLRC drc = wglCreateContext(ddc); wglMakeCurrent(ddc, drc);
    createAttribs = (PFNWGLCREATECTXATTRIBS)wglGetProcAddress("wglCreateContextAttribsARB");
    choosePf = (PFNWGLCHOOSEPIXELFMT)wglGetProcAddress("wglChoosePixelFormatARB");
    wglMakeCurrent(nullptr, nullptr); wglDeleteContext(drc); ReleaseDC(dw, ddc); DestroyWindow(dw);
    return createAttribs && choosePf;
}

static void* gl_sym(HMODULE glMod, const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    // wglGetProcAddress returns null (or 1/-1) for GL 1.1 symbols -> fall back to opengl32.dll.
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1)
        p = (void*)GetProcAddress(glMod, name);
    return p;
}

bool GLContext::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    destroy();
    w_ = w; h_ = h;
    PFNWGLCREATECTXATTRIBS createAttribs = nullptr; PFNWGLCHOOSEPIXELFMT choosePf = nullptr;
    if (!load_wgl_arb(createAttribs, choosePf)) { err = "no WGL ARB (need OpenGL 3.3 driver)"; return false; }

    if (native_window) { hwnd_ = (HWND)native_window; owns_window_ = false; }
    else { hwnd_ = make_hidden_window(); owns_window_ = true; }
    if (!hwnd_) { err = "no window"; return false; }
    hdc_ = GetDC(hwnd_);

    const int pfAttribs[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 32, WGL_ALPHA_BITS_ARB, 8,
        WGL_DEPTH_BITS_ARB, 24, 0 };
    int pf = 0; UINT n = 0;
    if (!choosePf(hdc_, pfAttribs, nullptr, 1, &pf, &n) || n == 0) { err = "no pixel format"; return false; }
    PIXELFORMATDESCRIPTOR pfd{}; pfd.nSize = sizeof(pfd);
    DescribePixelFormat(hdc_, pf, sizeof(pfd), &pfd);
    if (!SetPixelFormat(hdc_, pf, &pfd)) { err = "SetPixelFormat"; return false; }

    const int ctxAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    hglrc_ = createAttribs(hdc_, nullptr, ctxAttribs);
    if (!hglrc_) { err = "no 3.3-core context"; return false; }
    if (!wglMakeCurrent(hdc_, hglrc_)) { err = "wglMakeCurrent"; return false; }

    HMODULE glMod = GetModuleHandleA("opengl32.dll");
    #define LD(field, name) fns_.field = (decltype(fns_.field))gl_sym(glMod, name); if (!fns_.field) { err = std::string("missing GL entrypoint ") + name; return false; }
    LD(GetString,"glGetString") LD(GetIntegerv,"glGetIntegerv") LD(Viewport,"glViewport")
    LD(Clear,"glClear") LD(ClearColor,"glClearColor") LD(GenTextures,"glGenTextures")
    LD(DeleteTextures,"glDeleteTextures") LD(BindTexture,"glBindTexture") LD(TexParameteri,"glTexParameteri")
    LD(TexImage2D,"glTexImage2D") LD(TexSubImage2D,"glTexSubImage2D") LD(PixelStorei,"glPixelStorei")
    LD(ReadPixels,"glReadPixels") LD(DrawArrays,"glDrawArrays")
    LD(GenFramebuffers,"glGenFramebuffers") LD(BindFramebuffer,"glBindFramebuffer")
    LD(FramebufferTexture2D,"glFramebufferTexture2D") LD(CheckFramebufferStatus,"glCheckFramebufferStatus")
    LD(GenVertexArrays,"glGenVertexArrays") LD(BindVertexArray,"glBindVertexArray")
    LD(CreateShader,"glCreateShader") LD(ShaderSource,"glShaderSource") LD(CompileShader,"glCompileShader")
    LD(GetShaderiv,"glGetShaderiv") LD(GetShaderInfoLog,"glGetShaderInfoLog")
    LD(CreateProgram,"glCreateProgram") LD(AttachShader,"glAttachShader") LD(LinkProgram,"glLinkProgram")
    LD(GetProgramiv,"glGetProgramiv") LD(GetProgramInfoLog,"glGetProgramInfoLog") LD(UseProgram,"glUseProgram")
    LD(DeleteShader,"glDeleteShader") LD(GetUniformLocation,"glGetUniformLocation") LD(Uniform1i,"glUniform1i")
    LD(ActiveTexture,"glActiveTexture")
    #undef LD
    return true;
}

bool GLContext::make_current() { return hdc_ && hglrc_ && wglMakeCurrent(hdc_, hglrc_); }
void GLContext::present() { if (windowed()) SwapBuffers(hdc_); }   // headless: readback handles sync
void GLContext::destroy() {
    if (hglrc_) { wglMakeCurrent(nullptr,nullptr); wglDeleteContext(hglrc_); hglrc_=nullptr; }
    if (hdc_ && hwnd_) ReleaseDC(hwnd_, hdc_); hdc_=nullptr;
    if (owns_window_ && hwnd_) DestroyWindow(hwnd_);
    hwnd_=nullptr; owns_window_=false;
}
GLContext::~GLContext() { destroy(); }

bool GLContext::probe() {
    GLContext c; std::string e; bool ok = c.initialize(nullptr, 16, 16, e); return ok;
}
} // namespace rp
```

- [ ] **Step 5: CMake.** Add `src/render/gl/GLContext.cpp` (and, ahead, `GLCompositor.cpp`, `GLBackend.cpp`)
  to the `retropark` library's sources, and `target_link_libraries(retropark PUBLIC opengl32 gdi32)` (Windows).
  Add `tests/test_gl_context.cpp` to the test target.
- [ ] **Step 6: Run the test** → PASS on this GPU (or graceful skip). Run:
  `cmake --build build --config Release --target retropark_tests && ./build/tests/Release/retropark_tests.exe -tc="gl context*"`.
- [ ] **Step 7: Commit** (`feat(gl): WGL 3.3-core GLContext + entrypoint loader + probe`).

---

### Task 2: `GLShaders.h` + `GLCompositor` — draw a textured fullscreen quad

**Files:**
- Create: `src/render/gl/GLShaders.h`, `src/render/gl/GLCompositor.h`, `src/render/gl/GLCompositor.cpp`
- Test: `tests/test_gl_compositor.cpp` (mirror `tests/test_vulkan_compositor.cpp`'s spirit)

**Interfaces:**
- Consumes: `rp::GLContext` (its `GLFns`).
- Produces: `rp::GLCompositor` — `bool initialize(const GLFns&, std::string& err)` (compile program, make VAO),
  `void draw(const GLFns&, GLuint tex)` (draw the quad sampling `tex` into the currently-bound framebuffer),
  `void destroy(const GLFns&)`.

- [ ] **Step 1: `GLShaders.h`** — a passthrough quad. Uses `gl_VertexID` to emit a fullscreen triangle
  (no VBO needed), flips V so the sampled texture (top-left origin upload) shows upright on screen:

```cpp
#pragma once
namespace rp {
inline const char* kGLVertSrc =
  "#version 330 core\n"
  "out vec2 vUV;\n"
  "void main(){\n"
  "  vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"   // (0,0)(2,0)(0,2) fullscreen tri
  "  vUV = vec2(p.x, 1.0 - p.y);\n"                          // flip V: texture row 0 is the top
  "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
  "}\n";
inline const char* kGLFragSrc =
  "#version 330 core\n"
  "in vec2 vUV; out vec4 oColor; uniform sampler2D uTex;\n"
  "void main(){ oColor = texture(uTex, vUV); }\n";
}
```

- [ ] **Step 2: Write the failing compositor test** (`tests/test_gl_compositor.cpp`): make a headless context,
  init the compositor, upload a solid-color texture, draw into a 16×16 FBO, `glReadPixels`, assert the color.

```cpp
#include <doctest/doctest.h>
#include "render/gl/GLContext.h"
#include "render/gl/GLCompositor.h"
#include <vector>
using namespace rp;
TEST_CASE("gl compositor: draws a texture into an FBO") {
    if (!GLContext::probe()) { WARN("no GL 3.3"); return; }
    std::string err; GLContext ctx; REQUIRE(ctx.initialize(nullptr,16,16,err)); REQUIRE(ctx.make_current());
    const GLFns& g = ctx.gl();
    GLCompositor comp; REQUIRE(comp.initialize(g, err));
    // A 2x2 solid magenta (255,0,255,255) source texture.
    GLuint tex=0; g.GenTextures(1,&tex); g.BindTexture(0x0DE1/*GL_TEXTURE_2D*/,tex);
    g.TexParameteri(0x0DE1,0x2801/*MIN_FILTER*/,0x2600/*NEAREST*/);
    g.TexParameteri(0x0DE1,0x2800/*MAG_FILTER*/,0x2600);
    unsigned char px[2*2*4]; for(int i=0;i<4;i++){px[i*4]=255;px[i*4+1]=0;px[i*4+2]=255;px[i*4+3]=255;}
    g.TexImage2D(0x0DE1,0,0x8058/*RGBA8*/,2,2,0,0x1908/*GL_RGBA*/,0x1401/*UNSIGNED_BYTE*/,px);
    // FBO target 16x16.
    GLuint fbo=0,dst=0; g.GenFramebuffers(1,&fbo); g.GenTextures(1,&dst);
    g.BindTexture(0x0DE1,dst); g.TexImage2D(0x0DE1,0,0x8058,16,16,0,0x1908,0x1401,nullptr);
    g.BindFramebuffer(0x8D40/*FRAMEBUFFER*/,fbo);
    g.FramebufferTexture2D(0x8D40,0x8CE0/*COLOR_ATTACHMENT0*/,0x0DE1,dst,0);
    REQUIRE(g.CheckFramebufferStatus(0x8D40)==0x8CD5/*FRAMEBUFFER_COMPLETE*/);
    g.Viewport(0,0,16,16);
    comp.draw(g, tex);
    std::vector<unsigned char> out(16*16*4,0);
    g.ReadPixels(0,0,16,16,0x1908,0x1401,out.data());
    CHECK(out[0]==255); CHECK(out[1]==0); CHECK(out[2]==255);   // magenta filled the FBO
}
```

Run → FAIL (no `GLCompositor.h`).

- [ ] **Step 3: `GLCompositor.h`** (small):

```cpp
#pragma once
#include "render/gl/GLContext.h"
#include <string>
namespace rp {
class GLCompositor {
public:
    bool initialize(const GLFns& g, std::string& err);
    void draw(const GLFns& g, GLuint tex);   // into the currently-bound framebuffer + viewport
    void destroy(const GLFns& g);
private:
    GLuint prog_ = 0, vao_ = 0; GLint uTex_ = -1;
};
}
```

- [ ] **Step 4: `GLCompositor.cpp`** — compile+link `kGLVertSrc`/`kGLFragSrc` (report the info-log on
  failure), make an empty VAO (core profile requires a bound VAO for `glDrawArrays`), and `draw` = useProgram
  + bind VAO + bind `tex` to unit 0 + `DrawArrays(GL_TRIANGLES, 0, 3)`.

```cpp
#include "render/gl/GLCompositor.h"
#include "render/gl/GLShaders.h"
namespace rp {
static GLuint compile(const GLFns& g, GLenum type, const char* src, std::string& err) {
    GLuint s = g.CreateShader(type); g.ShaderSource(s,1,&src,nullptr); g.CompileShader(s);
    GLint ok=0; g.GetShaderiv(s,0x8B81/*COMPILE_STATUS*/,&ok);
    if(!ok){ char log[512]; GLsizei n=0; g.GetShaderInfoLog(s,512,&n,log); err.assign(log,n); return 0; }
    return s;
}
bool GLCompositor::initialize(const GLFns& g, std::string& err) {
    GLuint vs=compile(g,0x8B31/*VERTEX_SHADER*/,kGLVertSrc,err); if(!vs) return false;
    GLuint fs=compile(g,0x8B30/*FRAGMENT_SHADER*/,kGLFragSrc,err); if(!fs) return false;
    prog_=g.CreateProgram(); g.AttachShader(prog_,vs); g.AttachShader(prog_,fs); g.LinkProgram(prog_);
    GLint ok=0; g.GetProgramiv(prog_,0x8B82/*LINK_STATUS*/,&ok);
    g.DeleteShader(vs); g.DeleteShader(fs);
    if(!ok){ char log[512]; GLsizei n=0; g.GetProgramInfoLog(prog_,512,&n,log); err.assign(log,n); return false; }
    uTex_=g.GetUniformLocation(prog_,"uTex");
    g.GenVertexArrays(1,&vao_);
    return true;
}
void GLCompositor::draw(const GLFns& g, GLuint tex) {
    g.UseProgram(prog_); g.BindVertexArray(vao_);
    g.ActiveTexture(0x84C0/*TEXTURE0*/); g.BindTexture(0x0DE1/*TEXTURE_2D*/,tex);
    if(uTex_>=0) g.Uniform1i(uTex_,0);
    g.DrawArrays(0x0004/*GL_TRIANGLES*/,0,3);
}
void GLCompositor::destroy(const GLFns&) { /* program/vao freed with the context */ }
}
```

- [ ] **Step 5: Run the test** → PASS (or skip). Commit (`feat(gl): GLCompositor — textured fullscreen-quad draw`).

---

### Task 3: `GLBackend` (`IRenderBackend`) + factory wiring — the driven green-field proof

**Files:**
- Create: `src/render/gl/GLBackend.h`, `src/render/gl/GLBackend.cpp`
- Modify: `src/runtime/BackendFactory.cpp`
- Test: add a `run_driven(RP_GFX_OPENGL)` case to `tests/test_driven_e2e.cpp`

**Interfaces:**
- Produces: `rp::GLBackend : IRenderBackend`, plus `static bool GLBackend::probe_gl_shared()` (delegates to
  `GLContext::probe()`) for tests.
- Consumes: `GLContext`, `GLCompositor`, `IRenderBackend`, `rp_surface_desc`, `RP_FMT_R8G8B8A8_UNORM`.

- [ ] **Step 1: `GLBackend.h`:**

```cpp
#pragma once
#include "render/IRenderBackend.h"
#include "render/gl/GLContext.h"
#include "render/gl/GLCompositor.h"
namespace rp {
class GLBackend : public IRenderBackend {
public:
    rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) override;
    rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                std::vector<rp_surface_desc>& out, std::string& err) override;
    rp_result composite_and_present(uint32_t, uint64_t, bool, uint8_t*, std::string& err) override;
    rp_result composite_driven(const void* data, uint32_t width, uint32_t height, uint32_t pitch,
                               bool dupe, uint8_t* out_rgba, std::string& err) override;
    static bool probe_gl_shared() { return GLContext::probe(); }
private:
    rp_result ensure_target(std::string& err);   // (re)create the headless FBO at width_/height_
    GLContext ctx_; GLCompositor comp_;
    GLuint frame_tex_ = 0;              // uploaded core frame
    GLuint fbo_ = 0, fbo_tex_ = 0;      // headless render target
    uint32_t width_ = 0, height_ = 0; bool headless_ = true; bool ready_ = false;
    uint32_t tex_w_ = 0, tex_h_ = 0;   // current frame_tex_ dims (realloc on change)
};
}
```

- [ ] **Step 2: `GLBackend.cpp`.** GL enum literals reused from the tasks above.

```cpp
#include "render/gl/GLBackend.h"
namespace rp {
enum { GL_TEXTURE_2D=0x0DE1, GL_RGBA8=0x8058, GL_RGBA=0x1908, GL_UBYTE=0x1401, GL_NEAREST=0x2600,
       GL_LINEAR=0x2601, GL_MIN=0x2801, GL_MAG=0x2800, GL_CLAMP=0x812F, GL_WRAP_S=0x2802, GL_WRAP_T=0x2803,
       GL_FRAMEBUFFER=0x8D40, GL_COLOR_ATTACH0=0x8CE0, GL_FB_COMPLETE=0x8CD5, GL_COLOR_BUFFER_BIT=0x4000,
       GL_UNPACK_ROW_LENGTH=0x0CF2, GL_PACK_ALIGNMENT=0x0D05, GL_UNPACK_ALIGNMENT=0x0CF5 };

rp_result GLBackend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_=w; height_=h; headless_=(native_window==nullptr); ready_=false;
    if (!ctx_.initialize(native_window,w,h,err)) return RP_ERR_DEVICE;
    if (!ctx_.make_current()) { err="make_current"; return RP_ERR_DEVICE; }
    if (!comp_.initialize(ctx_.gl(),err)) return RP_ERR_DEVICE;
    const GLFns& g=ctx_.gl();
    g.GenTextures(1,&frame_tex_);
    g.BindTexture(GL_TEXTURE_2D,frame_tex_);
    g.TexParameteri(GL_TEXTURE_2D,GL_MIN,GL_LINEAR); g.TexParameteri(GL_TEXTURE_2D,GL_MAG,GL_LINEAR);
    g.TexParameteri(GL_TEXTURE_2D,GL_WRAP_S,GL_CLAMP); g.TexParameteri(GL_TEXTURE_2D,GL_WRAP_T,GL_CLAMP);
    tex_w_=tex_h_=0;
    rp_result r=ensure_target(err); if(r!=RP_OK) return r;
    ready_=true; return RP_OK;
}

rp_result GLBackend::ensure_target(std::string& err) {
    if (!headless_) return RP_OK;                 // windowed draws to the default framebuffer
    const GLFns& g=ctx_.gl();
    if (!fbo_) g.GenFramebuffers(1,&fbo_);
    if (!fbo_tex_) g.GenTextures(1,&fbo_tex_);
    g.BindTexture(GL_TEXTURE_2D,fbo_tex_);
    g.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)width_,(GLsizei)height_,0,GL_RGBA,GL_UBYTE,nullptr);
    g.BindFramebuffer(GL_FRAMEBUFFER,fbo_);
    g.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACH0,GL_TEXTURE_2D,fbo_tex_,0);
    if (g.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FB_COMPLETE){ err="incomplete FBO"; return RP_ERR_DEVICE; }
    return RP_OK;
}

// Driven cores don't use shared surfaces; return inert descs so Runtime::resize -> rebuild_surfaces works.
rp_result GLBackend::allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                       std::vector<rp_surface_desc>& out, std::string& err) {
    (void)err; width_=w; height_=h; out.clear();
    // Resize may have changed w/h: recreate the headless target.
    if (ctx_.make_current()) { std::string e; ensure_target(e); }
    for (uint32_t i=0;i<count;++i){ rp_surface_desc d{}; d.index=i; d.width=w; d.height=h;
        d.format=RP_FMT_R8G8B8A8_UNORM; d.shared_handle=nullptr; d.generation=0; out.push_back(d); }
    return RP_OK;
}

rp_result GLBackend::composite_and_present(uint32_t, uint64_t, bool, uint8_t*, std::string& err) {
    err="OpenGL host does not support presenting cores"; return RP_ERR_UNSUPPORTED;   // never reached (api gate)
}

rp_result GLBackend::composite_driven(const void* data, uint32_t width, uint32_t height, uint32_t pitch,
                                      bool dupe, uint8_t* out_rgba, std::string& err) {
    if (!ready_) { err="not initialized"; return RP_ERR_DEVICE; }
    if (!ctx_.make_current()) { err="make_current"; return RP_ERR_DEVICE; }
    const GLFns& g=ctx_.gl();
    if (!dupe && data) {
        g.BindTexture(GL_TEXTURE_2D,frame_tex_);
        g.PixelStorei(GL_UNPACK_ALIGNMENT,1);
        g.PixelStorei(GL_UNPACK_ROW_LENGTH,(GLint)(pitch/4));   // pitch is bytes; RGBA8 => /4 texels
        if (width!=tex_w_||height!=tex_h_){
            g.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)width,(GLsizei)height,0,GL_RGBA,GL_UBYTE,data);
            tex_w_=width; tex_h_=height;
        } else {
            g.TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)width,(GLsizei)height,GL_RGBA,GL_UBYTE,data);
        }
        g.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
    }
    // Draw into the target (headless FBO / windowed default framebuffer).
    g.BindFramebuffer(GL_FRAMEBUFFER, headless_?fbo_:0);
    g.Viewport(0,0,(GLsizei)width_,(GLsizei)height_);
    g.ClearColor(0,0,0,1); g.Clear(GL_COLOR_BUFFER_BIT);
    if (tex_w_) comp_.draw(g, frame_tex_);
    if (headless_ && out_rgba) {
        g.PixelStorei(GL_PACK_ALIGNMENT,1);
        std::vector<uint8_t> flip((size_t)width_*height_*4);
        g.ReadPixels(0,0,(GLsizei)width_,(GLsizei)height_,GL_RGBA,GL_UBYTE,flip.data());
        // GL is bottom-left origin; out_rgba is top-left -> flip rows.
        const size_t row=(size_t)width_*4;
        for (uint32_t y=0;y<height_;++y)
            memcpy(out_rgba+(size_t)y*row, flip.data()+(size_t)(height_-1-y)*row, row);
    } else {
        ctx_.present();   // SwapBuffers
    }
    return RP_OK;
}
} // namespace rp
```

- [ ] **Step 3: Factory.** In `src/runtime/BackendFactory.cpp` add the include + case:

```cpp
#include "render/gl/GLBackend.h"
// ...
case RP_GFX_OPENGL: return std::make_unique<GLBackend>();
```

- [ ] **Step 4: Green-field e2e.** In `tests/test_driven_e2e.cpp`, add (include `render/gl/GLBackend.h`):

```cpp
TEST_CASE("driven e2e: OpenGL") {
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no OpenGL 3.3"); return; }
    run_driven(RP_GFX_OPENGL);                                   // green shows in the BR quadrant readback
}
TEST_CASE("driven e2e: resize a loaded core (OpenGL)") {
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no OpenGL 3.3"); return; }
    rp_runtime* rt=rp_runtime_create(RP_GFX_OPENGL,nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt,64,64)==RP_OK);
    REQUIRE(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);
    CHECK(rp_runtime_resize(rt,96,72)==RP_OK);                   // resize path (rebuild_surfaces) must not fail
    std::vector<uint8_t> img(96*72*4,0); bool green=false;
    for(int i=0;i<10&&!green;i++){ if(rp_runtime_present(rt,img.data())==RP_OK &&
        img[(((size_t)(72-4))*96+(96-4))*4+1]>150) green=true; }
    CHECK(green);
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
```

- [ ] **Step 5: Build + run** the driven suite → OpenGL cases PASS (or skip). Run:
  `./build/tests/Release/retropark_tests.exe -tc="driven e2e*"`.
- [ ] **Step 6: Commit** (`feat(gl): GLBackend driven-core composite + factory wiring (green-field e2e)`).

---

### Task 4: Real NES under the OpenGL host (gated e2e)

**Files:** Create `tests/test_gl_nes_e2e.cpp` (mirror the FCEUmm driven e2e used for D3D11/Vulkan — find it
via `grep -rn "fceumm\|libretro_shim" tests/`), OR add a gated case to the existing libretro e2e that takes
the api. Reuse that file's ROM-path + shim-dir constants and its non-black/advancing assertions verbatim.

**Interfaces:** Consumes the software libretro shim (`cores/libretro_shim`) + FCEUmm, exactly as the existing
libretro driven e2e, only with `rp_runtime_create(RP_GFX_OPENGL, ...)`.

- [ ] **Step 1:** Write a gated test (`RP_RUN_GL_NES=1`) that: probe-skips without GL; creates an
  `RP_GFX_OPENGL` runtime; `resize`; `load_core(cores/libretro_shim)` + `load_content(<nes rom>)`; pumps
  `rp_runtime_present`; asserts the frame is **non-black and advancing** (frame N ≠ frame N+K) — copy the
  exact assertions + ROM path from the existing NES driven e2e.
- [ ] **Step 2:** Run with the gate set → PASS (real NES pixels composited through GL). Confirm it SKIPS
  cleanly with the gate unset and on a GL-less machine.
- [ ] **Step 3: Commit** (`test(gl): real NES (FCEUmm) composites through the OpenGL host (gated)`).

---

### Task 5: Windowed harness `--api gl` + probe/CI registration

**Files:** Modify `harness/windowed/main.cpp` (api parse); `run-headless-probes.sh` + the Windows CI job
(`.github/workflows/*.yml`) to run the new GL tests probe-guarded.

- [ ] **Step 1:** In the harness `--api` parse (find `"vulkan"`/`RP_GFX_VULKAN`), add `"gl"`/`"opengl"` →
  `RP_GFX_OPENGL`. Verify: `retropark_harness.exe --api gl --content <nes rom>` shows the game in the window
  (Release build). (Manual/visual — no automated assertion; this is the windowed sanity check.)
- [ ] **Step 2:** Register `test_gl_context` / `test_gl_compositor` / the GL driven+NES cases in
  `run-headless-probes.sh` the way the Vulkan probes are (findexe-guarded; a graceful skip when no GL device
  reaches ALL HEADLESS PROBES PASSED). Add them to the Windows CI job (the tests already skip on a GL-less
  runner via `probe_gl_shared()`, so no CI regression on GPU-less agents).
- [ ] **Step 3: Commit** (`feat(gl): harness --api gl + probe/CI registration`).

> **RetroPark slice close:** verify the full suite (`retropark_tests.exe`, all green incl. the new GL cases on
> this GPU), then merge to `main` + push (no AI attribution). The RetroPark half of Subsystem A is done here;
> Tasks 6–7 are in the EverythingBox repo.

---

### Task 6: EverythingBox integration — user-selectable "RetroPark driven backend"

**Repo:** EverythingBox (`C:\Users\cubma\Project Goliath`). **Do NOT edit the shared tree directly for the
commit** — build/verify in a throwaway worktree off `origin/main` (see [[goliath-tree-is-shared]]).

**Files:** `native/src/emu/RetroParkRuntimeApi.h` (the api resolver); both settings surfaces in
`native/src/.../MainWindow.cpp` (the themed builder AND the QWidget builder — the two-settings-builders rule,
or the setting is unreachable); the RetroPark settings read path. All under `#ifdef EB_HAVE_RETROPARK`.

**Interfaces:**
- `rpapi::runtimeApiForCore(kind, /*presenting*/bool)` currently returns `RP_GFX_D3D11` for driven. Change the
  driven return to a resolver input: `rpapi::drivenApiFor(Settings)` → `RP_GFX_OPENGL` if the user picked
  OpenGL, else `RP_GFX_D3D11` (default). Presenting stays Vulkan unconditionally.

- [ ] **Step 1:** Bump EB's `external/RetroPark` submodule to the RetroPark commit that closed Task 5 (the
  worktree flow: `git worktree add ... origin/main`, `git submodule update --init external/RetroPark`, then
  `git -C external/RetroPark fetch && checkout <sha>`). `RETROPARK_ABI_VERSION` is unchanged (7) — no vehicle
  rebuild needed; the GL backend is pure host code compiled into `retropark.lib`.
- [ ] **Step 2:** Add the setting **"RetroPark driven backend: D3D11 (default) / OpenGL"** to BOTH settings
  builders (mirror the existing emulation/backend setting precedent), stored under the RetroPark settings
  namespace. Wire `RetroParkView`'s runtime creation to `rpapi::drivenApiFor(settings)` for the driven path;
  leave the presenting path (gc → Dolphin → Vulkan) untouched. **Default D3D11 — no regression to the shipped
  NES path.**
- [ ] **Step 3:** Build EB Release in the worktree (wipe `build/retropark_ext-prefix` stamps so
  `retropark.lib` relinks against the new submodule — see [[retropark-eb-integration]] ABI-bump gotcha, though
  here it's a source change not an ABI change). Confirm 0 errors.
- [ ] **Step 4: Live gate (`EB_UITEST`).** Launch the built exe, set the driven backend to OpenGL, launch a
  NES game on the RetroPark backend, confirm it renders (capture a frame). Set back to D3D11, confirm still
  works. (See [[verify-app-gui-capture]].)
- [ ] **Step 5: Commit** the submodule bump + EB changes in the worktree; push to `main`
  (`feat: RetroPark OpenGL driven backend selectable in Emulation settings`).

---

### Task 7: Deploy to `C:\EverythingBox-app`

- [ ] **Step 1:** From the worktree `build/Release`, **targeted-copy** `EverythingBox.exe` (+ `.pdb`) to
  `C:\EverythingBox-app` — **NOT `robocopy /MIR`** (it would delete the app's downloaded cores + savestates +
  settings). No new cores/DLLs to stage (GL is a system DLL; the GL backend ships inside the exe).
- [ ] **Step 2: Verify the deployed app:** launch `C:\EverythingBox-app\EverythingBox.exe`, switch the
  RetroPark driven backend to OpenGL, and confirm a NES game renders under it.
- [ ] **Step 3:** Update memory ([[retropark-project]] / [[retropark-eb-integration]]) with the OpenGL host
  backend landing + the Subsystem-B follow-up.

---

## Self-Review notes

- **Spec coverage:** GLContext (Task 1) / GLCompositor (Task 2) / GLBackend + factory + enum (Task 3) / NES
  (Task 4) / harness+CI (Task 5) / EB toggle (Task 6) / deploy (Task 7) — every spec section maps to a task.
- **`allocate_surfaces` returns `RP_OK` (inert descs)**, not UNSUPPORTED — the spec's corrected decision, so
  the driven resize path holds. Only `composite_and_present` is UNSUPPORTED.
- **No ABI bump** — `RP_GFX_OPENGL` is additive; the built vehicle DLLs (Dolphin) are untouched, so EB needs
  no vehicle rebuild, only a `retropark.lib` relink.
- **Type consistency:** `probe_gl_shared()` (GLBackend, tests' gate) delegates to `GLContext::probe()`; the
  `GLFns` table is the single source of GL entrypoints, passed by const-ref to the compositor.
- **Windowed present** is manual/visual only (no automated assertion) — GL windowed output can't be read back
  headlessly; the headless FBO path carries the automated proof.
