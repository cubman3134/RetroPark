#include <doctest/doctest.h>
#include <retropark/retropark.h>

TEST_CASE("runtime: create/resize/destroy headless") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    CHECK(rp_runtime_resize(rt, 64, 64) == RP_OK);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: loading a non-existent core dir fails cleanly") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    CHECK(rp_runtime_load_core(rt, "no_such_dir") == RP_ERR_NOT_FOUND);
    rp_runtime_destroy(rt);
}
