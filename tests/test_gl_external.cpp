#include <doctest/doctest.h>
#include "render/gl/GLBackend.h"
#include "render/gl/GLContext.h"
#include <vector>
using namespace rp;

// B2 zero-copy machinery: a SECOND GL context that SHARES with the host GLBackend's context uploads a
// known two-tone texture; composite_external_gl composites it directly (no CPU upload) and the readback
// must be UPRIGHT. This is the flip arbiter: origin bugs hide behind position-agnostic tests, so the
// texture mimics a bottom-origin HW-render FBO (texel row 0 = visual bottom) and we assert row 0 of the
// output is the visual-TOP color.
TEST_CASE("gl external: shared context + upright external-texture composite") {
    if (!GLBackend::probe_gl_shared()) { WARN("no GL 3.3 shared context"); return; }

    const uint32_t W = 8, H = 8;
    GLBackend backend; std::string err;
    REQUIRE(backend.initialize(nullptr, W, H, err) == RP_OK);   // headless

    // A second context sharing objects with the host's GL context.
    void* host_ctx = backend.gl_context();
    REQUIRE(host_ctx != nullptr);
    GLContext shared;
    REQUIRE(shared.initialize(nullptr, W, H, err, 3, 3, host_ctx));
    REQUIRE(shared.make_current());
    const GLFns& g = shared.gl();

    // A bottom-origin (GL) two-tone texture: visual TOP green, visual BOTTOM red. In a bottom-origin
    // FBO texel row 0 is the bottom -> lower texel rows red, upper texel rows green.
    std::vector<unsigned char> px((size_t)W * H * 4);
    for (uint32_t y = 0; y < H; ++y) {
        bool top = (y >= H / 2);   // higher texel rows = visual top = green
        for (uint32_t x = 0; x < W; ++x) {
            unsigned char* p = px.data() + ((size_t)y * W + x) * 4;
            p[0] = top ? 0   : 255;  // R
            p[1] = top ? 255 : 0;    // G
            p[2] = 0; p[3] = 255;
        }
    }
    GLuint tex = 0; g.GenTextures(1, &tex); g.BindTexture(0x0DE1 /*TEXTURE_2D*/, tex);
    g.TexParameteri(0x0DE1, 0x2801 /*MIN_FILTER*/, 0x2600 /*NEAREST*/);
    g.TexParameteri(0x0DE1, 0x2800 /*MAG_FILTER*/, 0x2600);
    g.TexImage2D(0x0DE1, 0, 0x8058 /*RGBA8*/, (GLsizei)W, (GLsizei)H, 0,
                 0x1908 /*GL_RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, px.data());

    // Composite the external (bottom-origin) texture directly through the host backend.
    std::vector<unsigned char> out((size_t)W * H * 4, 0);
    REQUIRE(backend.composite_external_gl(tex, W, H, /*bottom_left_origin*/true, out.data(), err) == RP_OK);

    // Output is top-origin: row 0 must be the visual TOP color (green); last row the bottom color (red).
    CHECK(out[1] > 200); CHECK(out[0] < 60);                    // row 0 green
    const unsigned char* last = out.data() + (size_t)(H - 1) * W * 4;
    CHECK(last[0] > 200); CHECK(last[1] < 60);                  // last row red
}

// uUVScale: a HW-render core allocates its FBO texture at MAX geometry but may render a smaller w*h frame
// into the bottom-left corner. composite_external_gl must sample only that valid w*h sub-region, not the
// whole texture -- else the frame corner-renders. Upload a 16x16 texture whose valid 8x8 bottom-left region
// is the two-tone image and whose remaining texels are a BLUE sentinel; compositing with w=h=8 must fill
// the 8x8 output with the two-tone image and NEVER show the blue sentinel.
TEST_CASE("gl external: sub-max frame samples only the valid w*h sub-region") {
    if (!GLBackend::probe_gl_shared()) { WARN("no GL 3.3 shared context"); return; }

    const uint32_t W = 8, H = 8;        // the rendered frame size (== output surface)
    const uint32_t TW = 16, TH = 16;    // the larger max-geometry texture
    GLBackend backend; std::string err;
    REQUIRE(backend.initialize(nullptr, W, H, err) == RP_OK);   // headless, 8x8 output

    void* host_ctx = backend.gl_context();
    REQUIRE(host_ctx != nullptr);
    GLContext shared;
    REQUIRE(shared.initialize(nullptr, TW, TH, err, 3, 3, host_ctx));
    REQUIRE(shared.make_current());
    const GLFns& g = shared.gl();

    std::vector<unsigned char> px((size_t)TW * TH * 4);
    for (uint32_t y = 0; y < TH; ++y) {
        for (uint32_t x = 0; x < TW; ++x) {
            unsigned char* p = px.data() + ((size_t)y * TW + x) * 4;
            if (x < W && y < H) {              // the valid bottom-left w*h region: same two-tone as above
                bool top = (y >= H / 2);       // higher texel rows = visual top = green
                p[0] = top ? 0 : 255; p[1] = top ? 255 : 0; p[2] = 0; p[3] = 255;
            } else {                           // outside the frame -> blue sentinel, must never be sampled
                p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255;
            }
        }
    }
    GLuint tex = 0; g.GenTextures(1, &tex); g.BindTexture(0x0DE1, tex);
    g.TexParameteri(0x0DE1, 0x2801, 0x2600); g.TexParameteri(0x0DE1, 0x2800, 0x2600);   // NEAREST
    g.TexImage2D(0x0DE1, 0, 0x8058, (GLsizei)TW, (GLsizei)TH, 0, 0x1908, 0x1401, px.data());

    std::vector<unsigned char> out((size_t)W * H * 4, 0);
    REQUIRE(backend.composite_external_gl(tex, W, H, /*bottom_left_origin*/true, out.data(), err) == RP_OK);

    // The blue sentinel must appear NOWHERE -> only the valid 8x8 sub-region of the 16x16 texture was sampled.
    bool anyBlue = false;
    for (size_t i = 0; i < out.size(); i += 4) if (out[i + 2] > 100) anyBlue = true;
    CHECK_FALSE(anyBlue);
    // Orientation preserved within the sub-region: row 0 green (visual top), last row red (visual bottom).
    CHECK(out[1] > 200); CHECK(out[0] < 60);
    const unsigned char* last = out.data() + (size_t)(H - 1) * W * 4;
    CHECK(last[0] > 200); CHECK(last[1] < 60);
}
