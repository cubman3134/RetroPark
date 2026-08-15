#include <doctest/doctest.h>
#include "render/gl/GLContext.h"
using namespace rp;

TEST_CASE("gl context: probe + make a 3.3-core context") {
    if (!GLContext::probe()) { WARN("no capable OpenGL 3.3 context; skipping"); return; }
    std::string err;
    GLContext ctx;
    REQUIRE(ctx.initialize(/*native_window=*/nullptr, 64, 64, err));   // headless
    REQUIRE(ctx.make_current());
    // GL_VERSION must be >= 3.3 core.
    const char* ver = (const char*)ctx.gl().GetString(0x1F02 /*GL_VERSION*/);
    REQUIRE(ver != nullptr);
    int major = 0, minor = 0;
    ctx.gl().GetIntegerv(0x821B /*GL_MAJOR_VERSION*/, &major);
    ctx.gl().GetIntegerv(0x821C /*GL_MINOR_VERSION*/, &minor);
    CHECK((major > 3 || (major == 3 && minor >= 3)));
}
