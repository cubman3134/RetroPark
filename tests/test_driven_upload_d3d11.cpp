#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <vector>
using namespace rp;

TEST_CASE("d3d11 driven: uploaded framebuffer (padded pitch) shows + overlay blends") {
    const uint32_t W=64,H=64,PITCH=W*4 + 16;   // 16 bytes row padding
    D3D11Backend b; std::string err;
    REQUIRE(b.initialize(nullptr, W, H, err) == RP_OK);
    std::vector<uint8_t> src((size_t)PITCH*H, 0);
    for (uint32_t y=0;y<H;y++) for (uint32_t x=0;x<W;x++) {
        uint8_t* p = src.data() + (size_t)y*PITCH + (size_t)x*4;
        p[0]=0; p[1]=255; p[2]=0; p[3]=255;   // solid green, padding left 0
    }
    std::vector<uint8_t> out((size_t)W*H*4, 0);
    REQUIRE(b.composite_driven(src.data(), W, H, PITCH, /*dupe=*/false, out.data(), err) == RP_OK);
    auto at=[&](std::vector<uint8_t>& buf, uint32_t x,uint32_t y,int c){ return buf[((size_t)y*W+x)*4+c]; };
    CHECK(at(out,60,60,1) > 200); CHECK(at(out,60,60,2) < 60);      // green outside overlay
    CHECK(at(out,4,4,2) > 80);    CHECK(at(out,4,4,1) < at(out,60,60,1)); // blended inside overlay

    // dupe==true must reuse the last uploaded texture (no new data supplied) and still
    // composite correctly: green outside the overlay, blend inside it.
    std::vector<uint8_t> out2((size_t)W*H*4, 0);
    REQUIRE(b.composite_driven(nullptr, W, H, 0, /*dupe=*/true, out2.data(), err) == RP_OK);
    CHECK(at(out2,60,60,1) > 200); CHECK(at(out2,60,60,2) < 60);
    CHECK(at(out2,4,4,2) > 80);    CHECK(at(out2,4,4,1) < at(out2,60,60,1));
}
