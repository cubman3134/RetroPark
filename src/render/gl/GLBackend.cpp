#include "render/gl/GLBackend.h"
#include <cstring>
namespace rp {
enum { GL_TEXTURE_2D=0x0DE1, GL_RGBA8=0x8058, GL_RGBA=0x1908, GL_UBYTE=0x1401, GL_NEAREST=0x2600,
       GL_LINEAR=0x2601, GL_MIN=0x2801, GL_MAG=0x2800, GL_CLAMP=0x812F, GL_WRAP_S=0x2802, GL_WRAP_T=0x2803,
       GL_FRAMEBUFFER=0x8D40, GL_COLOR_ATTACH0=0x8CE0, GL_FB_COMPLETE=0x8CD5, GL_COLOR_BUFFER_BIT=0x4000,
       GL_UNPACK_ROW_LENGTH=0x0CF2, GL_PACK_ALIGNMENT=0x0D05, GL_UNPACK_ALIGNMENT=0x0CF5 };

rp_result GLBackend::initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) {
    width_=w; height_=h; headless_=(native_window==nullptr); ready_=false;
    // A fresh context invalidates every prior GL name; drop stale handles so ensure_target
    // regenerates them (initialize is called again by Runtime::resize on an unloaded core).
    frame_tex_=0; fbo_=0; fbo_tex_=0;
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
    // Draw into the target (headless FBO / windowed default framebuffer). Headless uses the
    // core's native render size (width_/height_) so the readback matches; windowed scales the
    // fullscreen triangle to the actual window client area so the game fills the window (LINEAR
    // filtering upscales smoothly), matching the D3D11/Vulkan windowed backends.
    g.BindFramebuffer(GL_FRAMEBUFFER, headless_?fbo_:0);
    GLsizei vw=(GLsizei)width_, vh=(GLsizei)height_;
    if (!headless_) { uint32_t cw=0,ch=0; ctx_.client_size(cw,ch); if (cw && ch) { vw=(GLsizei)cw; vh=(GLsizei)ch; } }
    g.Viewport(0,0,vw,vh);
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

void* GLBackend::gl_context() const { return ctx_.hglrc(); }

rp_result GLBackend::composite_external_gl(unsigned tex, uint32_t w, uint32_t h,
                                           bool bottom_left_origin, uint8_t* out_rgba, std::string& err) {
    if (!ready_) { err="not initialized"; return RP_ERR_DEVICE; }
    if (!ctx_.make_current()) { err="make_current"; return RP_ERR_DEVICE; }
    const GLFns& g=ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, headless_?fbo_:0);
    GLsizei vw=(GLsizei)width_, vh=(GLsizei)height_;
    if (!headless_) { uint32_t cw=0,ch=0; ctx_.client_size(cw,ch); if (cw&&ch){ vw=(GLsizei)cw; vh=(GLsizei)ch; } }
    g.Viewport(0,0,vw,vh);
    g.ClearColor(0,0,0,1); g.Clear(GL_COLOR_BUFFER_BIT);
    // CPU-upload frames are top-origin -> flipV=1 (see composite_driven). An external HW-render FBO texture is
    // GL bottom-origin -> the OPPOSITE flip. So flipV = bottom_left_origin ? 0 : 1. (Pinned by the flip test.)
    comp_.draw(g, (GLuint)tex, bottom_left_origin ? 0 : 1);
    (void)w; (void)h;   // the texture carries its own size; we composite into the host surface (width_/height_)
    if (headless_ && out_rgba) {
        g.PixelStorei(GL_PACK_ALIGNMENT,1);
        std::vector<uint8_t> flip((size_t)width_*height_*4);
        g.ReadPixels(0,0,(GLsizei)width_,(GLsizei)height_,GL_RGBA,GL_UBYTE,flip.data());
        const size_t row=(size_t)width_*4;
        for (uint32_t y=0;y<height_;++y)
            memcpy(out_rgba+(size_t)y*row, flip.data()+(size_t)(height_-1-y)*row, row);
    } else {
        ctx_.present();
    }
    return RP_OK;
}
} // namespace rp
