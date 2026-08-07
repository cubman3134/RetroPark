#include <doctest/doctest.h>
#include "PixelConvert.h"
#include "libretro.h"
#include <vector>
#include <cstdint>
using rp::convert_to_rgba8;

TEST_CASE("convert: RGB565 pure colors -> RGBA8") {
    // one pixel each: red=0xF800, green=0x07E0, blue=0x001F
    uint16_t src[3] = {0xF800, 0x07E0, 0x001F};
    uint8_t dst[3*4] = {0};
    convert_to_rgba8(src, 3, 1, 3*2, RETRO_PIXEL_FORMAT_RGB565, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0);   CHECK(dst[2]==0);   CHECK(dst[3]==255); // red
    CHECK(dst[4]==0);   CHECK(dst[5]==255); CHECK(dst[6]==0);   CHECK(dst[7]==255); // green
    CHECK(dst[8]==0);   CHECK(dst[9]==0);   CHECK(dst[10]==255);CHECK(dst[11]==255);// blue
}
TEST_CASE("convert: 0RGB1555 pure colors -> RGBA8") {
    uint16_t src[3] = {0x7C00, 0x03E0, 0x001F}; // R,G,B in 5-5-5
    uint8_t dst[3*4] = {0};
    convert_to_rgba8(src, 3, 1, 3*2, RETRO_PIXEL_FORMAT_0RGB1555, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0);   CHECK(dst[2]==0);
    CHECK(dst[5]==255); CHECK(dst[10]==255);
}
TEST_CASE("convert: XRGB8888 -> RGBA8, respects source pitch padding") {
    // 2x2 XRGB8888 with 8 bytes row padding
    const uint32_t W=2,H=2,SP=2*4+8;
    std::vector<uint8_t> src(SP*H, 0xAA);
    auto put=[&](uint32_t x,uint32_t y,uint32_t v){ *reinterpret_cast<uint32_t*>(&src[y*SP+x*4])=v; };
    put(0,0,0x00FF0000); put(1,0,0x0000FF00); put(0,1,0x000000FF); put(1,1,0x00FFFFFF);
    uint8_t dst[W*H*4]={0};
    convert_to_rgba8(src.data(), W, H, SP, RETRO_PIXEL_FORMAT_XRGB8888, dst);
    CHECK(dst[0]==255); CHECK(dst[1]==0); CHECK(dst[2]==0);      // (0,0) red
    CHECK(dst[(2)*4+2]==255);                                    // (0,1) blue channel (src 0x000000FF)
    CHECK(dst[(3)*4+2]==255);                                    // (1,1) blue channel of white
}
