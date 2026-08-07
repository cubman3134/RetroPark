#pragma once
#include <cstdint>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch);
}
