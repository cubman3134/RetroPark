#include "render/FramebufferCopy.h"
#include <cstring>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch) {
    if (width == 0 || height == 0) return;
    const uint32_t row_bytes = width * 4u;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, row_bytes);
}

bool driven_frame_valid(uint32_t width, uint32_t height, uint32_t pitch,
                        uint32_t max_width, uint32_t max_height) {
    if (width == 0 || height == 0) return false;
    if (pitch < width * 4u) return false;
    if (width > max_width || height > max_height) return false;
    return true;
}
}
