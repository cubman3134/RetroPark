#pragma once
#include <cstdint>
namespace rp {
// libretro_format is a RETRO_PIXEL_FORMAT_* value. Writes width*height*4 RGBA8 bytes,
// tightly packed, dst[0..3] = R,G,B,255. Unknown format -> fills opaque black.
void convert_to_rgba8(const void* src, uint32_t width, uint32_t height,
                      uint32_t src_pitch, unsigned libretro_format, uint8_t* dst);
}
