#include <doctest/doctest.h>
#include "render/FramebufferCopy.h"
#include <vector>
using rp::copy_rgba8_rows;
using rp::driven_frame_valid;

TEST_CASE("fbcopy: respects source pitch padding") {
    // 2x2 image, source has 4 bytes row padding (src_pitch = 2*4 + 4 = 12), dst tightly packed (8).
    const uint32_t W=2,H=2,SRCP=12,DSTP=8;
    std::vector<uint8_t> src(SRCP*H, 0xCC);   // padding filled with 0xCC
    // row0 pixels
    for (int i=0;i<8;i++) src[i] = (uint8_t)i;
    // row1 pixels
    for (int i=0;i<8;i++) src[SRCP + i] = (uint8_t)(100+i);
    std::vector<uint8_t> dst(DSTP*H, 0);
    copy_rgba8_rows(src.data(), W, H, SRCP, dst.data(), DSTP);
    for (int i=0;i<8;i++) CHECK(dst[i] == (uint8_t)i);
    for (int i=0;i<8;i++) CHECK(dst[8+i] == (uint8_t)(100+i));
    // padding must NOT have been copied
    CHECK(dst.size() == 16);
}

TEST_CASE("driven_frame_valid: spec §4 frame-time input validation") {
    CHECK(driven_frame_valid(64, 64, 64*4, 320, 240) == true);
    CHECK(driven_frame_valid(64, 64, 64*4 - 1, 320, 240) == false);   // pitch < width*4
    CHECK(driven_frame_valid(400, 64, 400*4, 320, 240) == false);    // width > max_width
    CHECK(driven_frame_valid(64, 300, 64*4, 320, 240) == false);     // height > max_height
    CHECK(driven_frame_valid(0, 64, 64*4, 320, 240) == false);       // width == 0
}
