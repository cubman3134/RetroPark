#include "render/FramebufferCopy.h"
#include <cstring>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch) {
    const uint32_t row_bytes = width * 4u;
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, row_bytes);
}
}
