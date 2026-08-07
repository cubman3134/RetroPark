#pragma once
#include <cstdint>
namespace rp {
void copy_rgba8_rows(const uint8_t* src, uint32_t width, uint32_t height,
                     uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch);

// Spec §4 driven frame-time input validation: a frame with a pitch too small to hold
// width*4 bytes per row, or dimensions beyond the core's declared max geometry, must be
// rejected (treated as a duplicate of the last good frame) rather than uploaded/copied.
bool driven_frame_valid(uint32_t width, uint32_t height, uint32_t pitch,
                        uint32_t max_width, uint32_t max_height);
}
