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

TEST_CASE("runtime: failed load does not brick the runtime") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    CHECK(rp_runtime_load_core(rt, "no_such_dir") == RP_ERR_NOT_FOUND);
    CHECK(rp_runtime_load_core(rt, "no_such_dir") == RP_ERR_NOT_FOUND);
    // A runtime that failed to load a core twice must still be usable.
    CHECK(rp_runtime_resize(rt, 32, 32) == RP_OK);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: resize when no core is loaded still works") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    CHECK(rp_runtime_resize(rt, 48, 48) == RP_OK);
    CHECK(rp_runtime_resize(rt, 96, 96) == RP_OK);
    rp_runtime_destroy(rt);
}
