#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <retropark/retropark_abi.h>

namespace rp {
struct IRenderBackend {
    virtual ~IRenderBackend() = default;
    virtual rp_result initialize(void* native_window, uint32_t w, uint32_t h, std::string& err) = 0;
    virtual rp_result allocate_surfaces(uint32_t count, uint32_t w, uint32_t h,
                                        std::vector<rp_surface_desc>& out, std::string& err) = 0;
    // If out_rgba != null, the composited image (w*h*4, RGBA8) is copied there (headless).
    virtual rp_result composite_and_present(uint32_t ready_index, bool has_frame,
                                            uint8_t* out_rgba, std::string& err) = 0;
};
}
