#include "HwRenderGL.h"
#include <cstring>
namespace rp {
enum { GL_TEXTURE_2D=0x0DE1, GL_RGBA8=0x8058, GL_RGBA=0x1908, GL_UBYTE=0x1401, GL_NEAREST=0x2600,
       GL_MIN=0x2801, GL_MAG=0x2800, GL_FRAMEBUFFER=0x8D40, GL_COLOR_ATTACH0=0x8CE0, GL_DEPTH_ATTACH=0x8D00,
       GL_DEPTH_STENCIL_ATTACH=0x821A, GL_RENDERBUFFER=0x8D41, GL_DEPTH24=0x81A6, GL_DEPTH24_STENCIL8=0x88F0,
       GL_FB_COMPLETE=0x8CD5, GL_PACK_ALIGNMENT=0x0D05, GL_COLOR_BUFFER_BIT=0x4000, GL_SCISSOR_TEST=0x0C11 };

bool HwRenderGL::setup(bool depth, bool stencil, bool blo, uint32_t maxW, uint32_t maxH,
                       int major, int minor, void* share_context, std::string& err) {
    maxW_ = maxW ? maxW : 1; maxH_ = maxH ? maxH : 1; blo_ = blo;
    if (!ctx_.initialize(nullptr, maxW_, maxH_, err, major, minor, share_context)) {
        if (share_context) {   // B2: sharing the host's GL context failed -- degrade to B1 (standalone) readback
            if (!ctx_.initialize(nullptr, maxW_, maxH_, err, major, minor, nullptr)) return false;
        } else {
            return false;
        }
    } else {
        zero_copy_ = (share_context != nullptr);   // only true when the SHARED init succeeded
    }
    if (!ctx_.make_current()) { err = "make_current"; return false; }
    const GLFns& g = ctx_.gl();
    g.GenFramebuffers(1, &fbo_); g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    g.GenTextures(1, &color_); g.BindTexture(GL_TEXTURE_2D, color_);
    g.TexParameteri(GL_TEXTURE_2D, GL_MIN, GL_NEAREST); g.TexParameteri(GL_TEXTURE_2D, GL_MAG, GL_NEAREST);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)maxW_, (GLsizei)maxH_, 0, GL_RGBA, GL_UBYTE, nullptr);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACH0, GL_TEXTURE_2D, color_, 0);
    if (depth) {
        g.GenRenderbuffers(1, &depth_rb_); g.BindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        // Only-stencil is invalid per libretro; a packed 24/8 covers depth or depth+stencil.
        GLenum fmt = stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH24;
        GLenum att = stencil ? GL_DEPTH_STENCIL_ATTACH : GL_DEPTH_ATTACH;
        g.RenderbufferStorage(GL_RENDERBUFFER, fmt, (GLsizei)maxW_, (GLsizei)maxH_);
        g.FramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, depth_rb_);
    }
    if (g.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FB_COMPLETE) { err = "incomplete FBO"; return false; }
    return true;
}

const void* HwRenderGL::read_frame(uint32_t& w, uint32_t& h, uint32_t& out_pitch) {
    if (w > maxW_) w = maxW_; if (h > maxH_) h = maxH_;   // clamp the CALLER's w/h (crop, don't drop)
    out_pitch = w * 4;
    const GLFns& g = ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    read_.assign((size_t)w * h * 4, 0);
    g.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UBYTE, read_.data());
    if (!blo_) return read_.data();                 // core uses top-left origin: no flip
    flip_.assign((size_t)w * h * 4, 0);             // GL bottom-origin -> flip to top-origin
    const size_t row = (size_t)w * 4;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(flip_.data() + (size_t)y*row, read_.data() + (size_t)(h-1-y)*row, row);
    return flip_.data();
}

void HwRenderGL::test_fill(uint8_t tr,uint8_t tg,uint8_t tb, uint8_t br,uint8_t bg,uint8_t bb) {
    const GLFns& g = ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    // GL bottom half (y=0..h/2) = the visual BOTTOM; top half = visual TOP. Paint accordingly.
    g.Viewport(0,0,(GLsizei)maxW_,(GLsizei)maxH_);
    g.Enable(GL_SCISSOR_TEST);
    g.Scissor(0,0,(GLsizei)maxW_,(GLsizei)(maxH_/2));           // bottom half
    g.ClearColor(br/255.f,bg/255.f,bb/255.f,1.f); g.Clear(GL_COLOR_BUFFER_BIT);
    g.Scissor(0,(GLsizei)(maxH_/2),(GLsizei)maxW_,(GLsizei)(maxH_-maxH_/2)); // top half
    g.ClearColor(tr/255.f,tg/255.f,tb/255.f,1.f); g.Clear(GL_COLOR_BUFFER_BIT);
    g.Disable(GL_SCISSOR_TEST);
}
} // namespace rp
