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
