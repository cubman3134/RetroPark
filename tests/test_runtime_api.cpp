#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include <vector>
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

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

TEST_CASE("runtime: unsupported api yields a usable-but-erroring runtime, no crash") {
    rp_runtime* rt = rp_runtime_create((rp_graphics_api)99, nullptr);
    REQUIRE(rt != nullptr);
    CHECK(rp_runtime_resize(rt, 64, 64) == RP_ERR_DEVICE);
    CHECK(rp_runtime_present(rt, nullptr) == RP_ERR_DEVICE);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: load_content on a core without load_content is unsupported") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    // no core loaded yet -> content load has nothing to target
    CHECK(rp_runtime_load_content(rt, "whatever.nes") == RP_ERR_INTERNAL);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: load_content on a loaded driven core with no load_content fn is unsupported") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    // refcore_driven has a null load_content -> CoreLoader::load_content's null-fn
    // passthrough must surface as RP_ERR_UNSUPPORTED, not a crash or wrong code.
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
    CHECK(rp_runtime_load_content(rt, "x.nes") == RP_ERR_UNSUPPORTED);
    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime: audio stats are zero for a no-audio driven core") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // refcore_driven: sample_rate 0
    std::vector<uint8_t> img(64*64*4, 0);
    for (int i=0;i<5;i++) rp_runtime_present(rt, img.data());
    uint64_t frames = 999; int nonsilent = 9;
    rp_runtime_audio_stats(rt, &frames, &nonsilent);
    CHECK(frames == 0);          // no-audio core produced no samples
    CHECK(nonsilent == 0);
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
