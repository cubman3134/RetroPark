#include <doctest/doctest.h>
#include "../cores/libretro_shim/HwRenderGL.h"
#include "render/gl/GLContext.h"
using namespace rp;
TEST_CASE("hwrender gl: FBO readback is top-origin (flip works)") {
    if (!GLContext::probe()) { WARN("no GL 3.3"); return; }
    HwRenderGL hw; std::string err;
    REQUIRE(hw.setup(/*depth*/true,/*stencil*/false,/*bottom_left_origin*/true, 8, 8, 3, 3, err));
    REQUIRE(hw.make_current());
    hw.test_fill(0,255,0, 255,0,0);   // top green, bottom red (in visual/top-origin terms)
    uint32_t pitch = 0;
    const uint8_t* px = static_cast<const uint8_t*>(hw.read_frame(8, 8, pitch));
    REQUIRE(px); REQUIRE(pitch == 8*4);
    // Row 0 is the TOP of the image -> should be green; last row -> red.
    CHECK(px[1] > 200); CHECK(px[0] < 60);                       // row 0 green
    const uint8_t* last = px + (size_t)7*pitch;
    CHECK(last[0] > 200); CHECK(last[1] < 60);                   // row 7 red
}
