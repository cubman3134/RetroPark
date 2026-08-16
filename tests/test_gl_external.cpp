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
