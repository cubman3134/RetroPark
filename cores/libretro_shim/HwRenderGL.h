#pragma once
#include "render/gl/GLContext.h"
#include <vector>
#include <string>
#include <cstdint>
namespace rp {
// Owns the GL context + FBO a HW-render libretro core draws into, and reads it back to CPU RGBA. Isolated
// from LibretroShim's core-loading logic. Headless (its own GLContext) so it works under any host backend.
class HwRenderGL {
public:
    bool setup(bool depth, bool stencil, bool bottom_left_origin,
               uint32_t maxW, uint32_t maxH, int major, int minor, std::string& err);
    unsigned fbo_id() const { return fbo_; }              // for get_current_framebuffer
    bool make_current() { return ctx_.make_current(); }   // before each retro_run
    // Read the w*h region of the FBO to top-origin RGBA8; returns an internal buffer valid until the next
    // call. out_pitch = w*4. Flips rows iff the core uses bottom-left origin (GL default).
    const void* read_frame(uint32_t w, uint32_t h, uint32_t& out_pitch);
    uint32_t max_w() const { return maxW_; }
    uint32_t max_h() const { return maxH_; }
    // Test-only: paint the FBO's GL-top half to (tr,tg,tb) and GL-bottom half to (br,bg,bb) via scissor-clear,
    // mimicking a bottom-left-origin core so read_frame's flip can be exercised. Not used by the shim.
    void test_fill(uint8_t tr,uint8_t tg,uint8_t tb, uint8_t br,uint8_t bg,uint8_t bb);
private:
    GLContext ctx_;
    unsigned fbo_ = 0, color_ = 0, depth_rb_ = 0;
    uint32_t maxW_ = 0, maxH_ = 0;
    bool blo_ = true;
    std::vector<uint8_t> read_, flip_;
};
} // namespace rp
