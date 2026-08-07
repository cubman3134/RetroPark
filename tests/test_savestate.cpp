#include <doctest/doctest.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include <retropark/retropark.h>
#include <retropark/retropark_abi.h>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

using namespace rp;

// Portable, device-free round-trip against the real refcore_driven DLL (loaded directly
// through CoreLoader + Win32CoreModule, not rp_runtime — the driven core needs no D3D11/
// Vulkan device, so this stays a plain unit test). The full pixel-exact e2e (advance,
// serialize, advance further, unserialize, re-render, compare frames) is Task 3.
namespace {
void noop_log(rp_host*, int, const char*) {}
void noop_submit(rp_host*, uint32_t, uint64_t, uint64_t) {}
void noop_input(rp_host*, rp_input_state* out) { *out = rp_input_state{}; }
void noop_video(rp_host*, const void*, uint32_t, uint32_t, uint32_t) {}
void noop_audio(rp_host*, const int16_t*, size_t) {}

rp_result open_driven(std::unique_ptr<Win32CoreModule>& mod, CoreLoader& ld, std::string& err) {
    rp_result r = Win32CoreModule::open(std::string(RP_DRIVEN_CORE_DIR) + "/refcore_driven.dll", mod, err);
    if (r != RP_OK) return r;
    r = ld.load(mod.get(), err);
    if (r != RP_OK) return r;
    rp_host_iface host{};
    host.log = noop_log;
    host.submit_frame = noop_submit;
    host.input_state = noop_input;
    host.video_refresh = noop_video;
    host.audio_sample = noop_audio;
    return ld.create(&host, err);
}
}

TEST_CASE("refcore_driven: serialize_size is 4 bytes") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);
    CHECK(ld.serialize_size() == 4u);
    ld.destroy();
}

TEST_CASE("refcore_driven: serialize/unserialize round-trips the frame counter") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);

    for (int i = 0; i < 5; ++i) REQUIRE(ld.run_frame(err) == RP_OK);

    std::vector<uint8_t> snap(ld.serialize_size());
    REQUIRE(ld.serialize(snap.data(), snap.size(), err) == RP_OK);

    for (int i = 0; i < 10; ++i) REQUIRE(ld.run_frame(err) == RP_OK);

    std::vector<uint8_t> advanced(ld.serialize_size());
    REQUIRE(ld.serialize(advanced.data(), advanced.size(), err) == RP_OK);
    CHECK(advanced != snap);   // state actually moved on

    REQUIRE(ld.unserialize(snap.data(), snap.size(), err) == RP_OK);

    std::vector<uint8_t> restored(ld.serialize_size());
    REQUIRE(ld.serialize(restored.data(), restored.size(), err) == RP_OK);
    CHECK(restored == snap);   // back to the captured counter

    ld.destroy();
}

TEST_CASE("refcore_driven: serialize rejects undersized buffer / null data") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);

    uint8_t buf[4] = {0};
    CHECK(ld.serialize(buf, 3, err) == RP_ERR_BAD_ARG);
    CHECK(ld.serialize(nullptr, 4, err) == RP_ERR_BAD_ARG);
    CHECK(ld.unserialize(buf, 3, err) == RP_ERR_BAD_ARG);
    CHECK(ld.unserialize(nullptr, 4, err) == RP_ERR_BAD_ARG);

    ld.destroy();
}

// ---- Task 3: rp_runtime_* savestate C API + the portable, device-free e2e ----
//
// The runtime's D3D11 backend defaults to a WARP (software) device (see
// D3D11Backend::initialize) — the same reason test_driven_e2e.cpp's "driven e2e: D3D11"
// case is not probe-gated. That makes RP_GFX_D3D11 the portable, device-free path for a
// pixel-exact rp_runtime e2e: no real GPU is touched, only a software rasterizer, so this
// case runs unconditionally (unlike the Vulkan sibling, which probes for a loader first).

TEST_CASE("rp_runtime savestate: portable pixel-exact round-trip (D3D11/WARP, driven core)") {
    const uint32_t W = 64, H = 64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);

    std::vector<uint8_t> scratch((size_t)W * H * 4, 0);
    for (int i = 0; i < 3; ++i) REQUIRE(rp_runtime_present(rt, scratch.data()) == RP_OK);

    size_t sz = rp_runtime_serialize_size(rt);
    CHECK(sz == 4u);

    std::vector<uint8_t> state(sz);
    REQUIRE(rp_runtime_save_state(rt, state.data(), state.size()) == RP_OK);

    std::vector<uint8_t> r1((size_t)W * H * 4, 0);
    REQUIRE(rp_runtime_present(rt, r1.data()) == RP_OK);

    std::vector<uint8_t> advanced((size_t)W * H * 4, 0);
    for (int i = 0; i < 30; ++i) REQUIRE(rp_runtime_present(rt, advanced.data()) == RP_OK);
    CHECK(advanced != r1);   // state really moved on

    REQUIRE(rp_runtime_load_state(rt, state.data(), state.size()) == RP_OK);

    std::vector<uint8_t> r2((size_t)W * H * 4, 0);
    REQUIRE(rp_runtime_present(rt, r2.data()) == RP_OK);
    CHECK(r2 == r1);         // counter restored -> identical re-rendered frame

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}

TEST_CASE("rp_runtime savestate: negative cases") {
    const uint32_t W = 64, H = 64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);

    SUBCASE("no core loaded: serialize_size 0, save_state UNSUPPORTED, load_state does not crash") {
        CHECK(rp_runtime_serialize_size(rt) == 0u);
        uint8_t buf[4] = {0};
        CHECK(rp_runtime_save_state(rt, buf, sizeof(buf)) == RP_ERR_UNSUPPORTED);
        CHECK(rp_runtime_load_state(rt, buf, sizeof(buf)) != RP_OK);   // no crash, no false success
    }

    SUBCASE("null args") {
        CHECK(rp_runtime_serialize_size(nullptr) == 0u);
        uint8_t buf[4] = {0};
        CHECK(rp_runtime_save_state(nullptr, buf, sizeof(buf)) == RP_ERR_BAD_ARG);
        CHECK(rp_runtime_save_state(rt, nullptr, sizeof(buf)) == RP_ERR_BAD_ARG);
        CHECK(rp_runtime_load_state(nullptr, buf, sizeof(buf)) == RP_ERR_BAD_ARG);
        CHECK(rp_runtime_load_state(rt, nullptr, sizeof(buf)) == RP_ERR_BAD_ARG);
    }

    SUBCASE("undersized save buffer") {
        REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
        size_t sz = rp_runtime_serialize_size(rt);
        REQUIRE(sz == 4u);
        std::vector<uint8_t> small(sz - 1);
        CHECK(rp_runtime_save_state(rt, small.data(), small.size()) == RP_ERR_BAD_ARG);
        rp_runtime_unload_core(rt);
    }

    rp_runtime_destroy(rt);
}
