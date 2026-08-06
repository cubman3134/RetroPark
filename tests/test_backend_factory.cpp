#include <doctest/doctest.h>
#include "runtime/BackendFactory.h"
using namespace rp;
TEST_CASE("factory: d3d11 and vulkan produce backends; unknown is null") {
    CHECK(make_backend(RP_GFX_D3D11) != nullptr);
    CHECK(make_backend(RP_GFX_VULKAN) != nullptr);
    CHECK(make_backend((rp_graphics_api)99) == nullptr);
}
