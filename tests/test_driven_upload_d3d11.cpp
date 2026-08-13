#include <doctest/doctest.h>
#include "render/d3d11/D3D11Backend.h"
#include <vector>
using namespace rp;

TEST_CASE("d3d11 driven: uploaded framebuffer (padded pitch) composites into our surface") {
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
    CHECK(at(out,60,60,1) > 200); CHECK(at(out,60,60,2) < 60);      // uploaded green composites

    // dupe==true must reuse the last uploaded texture (no new data supplied) and still
    // composite the green frame correctly.
    std::vector<uint8_t> out2((size_t)W*H*4, 0);
    REQUIRE(b.composite_driven(nullptr, W, H, 0, /*dupe=*/true, out2.data(), err) == RP_OK);
    CHECK(at(out2,60,60,1) > 200); CHECK(at(out2,60,60,2) < 60);
}

TEST_CASE("d3d11 driven: core-res frame is scaled to fill a larger display target") {
    // Core frame is 32x32; the display/backbuffer is 128x96. The driven texture must be
    // scaled up to fill the display target (compositor samples with 0..1 UVs against
    // width_/height_), not rendered at core size into a corner of a larger target -- this
    // is the core-res != display-res case the reference core (which always matches) never
    // exercises.
    const uint32_t CW=32,CH=32,CPITCH=32*4;
    const uint32_t DW=128,DH=96;
    D3D11Backend b; std::string err;
    REQUIRE(b.initialize(nullptr, DW, DH, err) == RP_OK);
    std::vector<uint8_t> src((size_t)CPITCH*CH, 0);
    for (uint32_t y=0;y<CH;y++) for (uint32_t x=0;x<CW;x++) {
        uint8_t* p = src.data() + (size_t)y*CPITCH + (size_t)x*4;
        p[0]=0; p[1]=255; p[2]=0; p[3]=255;   // solid green
    }
    std::vector<uint8_t> out((size_t)DW*DH*4, 0);
    REQUIRE(b.composite_driven(src.data(), CW, CH, CPITCH, /*dupe=*/false, out.data(), err) == RP_OK);
    auto at=[&](uint32_t x,uint32_t y,int c){ return out[((size_t)y*DW+x)*4+c]; };
    // Bottom-right corner: only reachable by green if the 32x32 core frame was scaled
    // up to fill the full 128x96 display target.
    CHECK(at(120,90,1) > 200);
    CHECK(at(120,90,2) < 60);
}
