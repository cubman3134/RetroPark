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

bool GLContext::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err, int major, int minor,
                           void* share_context) {
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

    int cmaj = major, cmin = minor;
    if (cmaj < 3 || (cmaj == 3 && cmin < 3)) { cmaj = 3; cmin = 3; }
    const int ctxAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, cmaj, WGL_CONTEXT_MINOR_VERSION_ARB, cmin,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
    hglrc_ = createAttribs(hdc_, (HGLRC)share_context, ctxAttribs);
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
    LD(GenRenderbuffers,"glGenRenderbuffers") LD(BindRenderbuffer,"glBindRenderbuffer")
    LD(RenderbufferStorage,"glRenderbufferStorage") LD(FramebufferRenderbuffer,"glFramebufferRenderbuffer")
    LD(Enable,"glEnable") LD(Disable,"glDisable") LD(Scissor,"glScissor")
    #undef LD
    return true;
}

bool GLContext::make_current() { return hdc_ && hglrc_ && wglMakeCurrent(hdc_, hglrc_); }
void GLContext::client_size(uint32_t& w, uint32_t& h) const {
    RECT rc{};
    if (hwnd_ && GetClientRect(hwnd_, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
        w = (uint32_t)(rc.right - rc.left); h = (uint32_t)(rc.bottom - rc.top);
    } else { w = w_; h = h_; }   // fall back to the configured size
}
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
