#include <doctest/doctest.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include "runtime/RewindRing.h"
#include <retropark/retropark.h>
#include <retropark/retropark_abi.h>
#include <vector>
#include <deque>
#include <string>
#include <memory>
#include <cstdint>
#include <filesystem>

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
#ifndef RP_SHIM_DIR
#define RP_SHIM_DIR "cores/libretro_shim"
#endif
#ifndef RP_NES_ROM_DIR
#define RP_NES_ROM_DIR "C:/RetroBat/roms/nes"
#endif

// Duplicated (not shared) probe helpers, matching the sibling gated e2e files
// (test_libretro_e2e.cpp, test_audio_flow.cpp) which each keep their own
// prefixed static copy rather than a cross-TU header for two tiny functions.
static bool savestate_file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string savestate_first_nes(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".nes") return entry.path().string();
    }
    return {};
}

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

// ---- Task 4: rewind ring (bookkeeping unit + portable frame-by-frame e2e) ----

// A. Pure ring-bookkeeping unit (no device): pushing past max drops the oldest, caps at max,
//    order preserved; under capacity nothing is dropped.
TEST_CASE("rewind_ring_push: bounded drop-oldest keeps newest, order preserved") {
    auto mk = [](uint8_t v) { return std::vector<uint8_t>{v}; };

    std::deque<std::vector<uint8_t>> ring;
    const uint32_t MAX = 3;
    for (uint8_t v = 0; v < 6; ++v) rewind_ring_push(ring, mk(v), MAX);
    REQUIRE(ring.size() == MAX);                 // capped at max
    CHECK(ring.front()[0] == 3);                 // oldest survivor
    CHECK(ring.back()[0] == 5);                  // newest
    CHECK(ring[0][0] == 3);                       // order preserved: 3,4,5
    CHECK(ring[1][0] == 4);
    CHECK(ring[2][0] == 5);

    std::deque<std::vector<uint8_t>> under;
    rewind_ring_push(under, mk(9), 4);
    rewind_ring_push(under, mk(8), 4);
    CHECK(under.size() == 2u);                    // under capacity: nothing dropped
    CHECK(under.front()[0] == 9);
    CHECK(under.back()[0] == 8);
}

// B. Portable rewind e2e (refcore_driven, D3D11/WARP, device-free — same harness as the
//    savestate e2e). refcore_driven renders each frame from its counter then increments, so a
//    snapshot is the value the NEXT run_frame renders. This proves frame-by-frame visual rewind
//    against real pixel readbacks: rewind+present shows the SAME pixels as an earlier forward
//    frame, consecutive rewinds step monotonically backward, and a forward present after a
//    rewind resumes both forward motion AND capture.
TEST_CASE("rp_runtime rewind: portable frame-by-frame backward stepping (D3D11/WARP, driven core)") {
    const uint32_t W = 64, H = 64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);

    REQUIRE(rp_runtime_set_rewind(rt, 1, 64) == RP_OK);

    // Run M forward frames (capturing), recording each displayed frame's pixels.
    const int M = 10;
    std::vector<std::vector<uint8_t>> forward;
    for (int i = 0; i < M; ++i) {
        std::vector<uint8_t> f((size_t)W * H * 4, 0);
        REQUIRE(rp_runtime_present(rt, f.data()) == RP_OK);
        forward.push_back(std::move(f));
    }
    CHECK(forward.front() != forward.back());     // frames really animate

    // Rewind K times; each rewind()+present() must display exactly one earlier forward frame.
    // Last forward-displayed was forward[M-1]; rewind #k re-renders forward[M-1-k] — monotonic.
    const int K = 5;
    for (int k = 1; k <= K; ++k) {
        REQUIRE(rp_runtime_rewind(rt) == RP_OK);
        std::vector<uint8_t> r((size_t)W * H * 4, 0);
        REQUIRE(rp_runtime_present(rt, r.data()) == RP_OK);
        CHECK(r == forward[M - 1 - k]);            // stepped back exactly one
    }
    // Now displaying forward[M-1-K] == forward[4].

    // Step 5: resume a forward present (no rewind) -> advances forward again AND resumes capture.
    std::vector<uint8_t> fwd((size_t)W * H * 4, 0);
    REQUIRE(rp_runtime_present(rt, fwd.data()) == RP_OK);
    CHECK(fwd == forward[M - K]);                  // forward[5]: one step FORWARD from forward[4]

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}

// C. Rewind negative + guard cases.
TEST_CASE("rp_runtime rewind: negative + guard cases") {
    const uint32_t W = 64, H = 64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);

    SUBCASE("null rt") {
        CHECK(rp_runtime_set_rewind(nullptr, 1, 64) == RP_ERR_BAD_ARG);
        CHECK(rp_runtime_rewind(nullptr) == RP_ERR_BAD_ARG);
    }

    SUBCASE("set_rewind with no serialize-capable core -> UNSUPPORTED") {
        CHECK(rp_runtime_set_rewind(rt, 1, 64) == RP_ERR_UNSUPPORTED);
    }

    SUBCASE("rewind while disabled -> INTERNAL") {
        REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
        CHECK(rp_runtime_rewind(rt) == RP_ERR_INTERNAL);   // enabled never set
        rp_runtime_unload_core(rt);
    }

    SUBCASE("rewind with too little history -> NOT_FOUND, no crash") {
        REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);
        REQUIRE(rp_runtime_set_rewind(rt, 1, 64) == RP_OK);
        CHECK(rp_runtime_rewind(rt) == RP_ERR_NOT_FOUND);  // empty ring
        std::vector<uint8_t> f((size_t)W * H * 4, 0);
        REQUIRE(rp_runtime_present(rt, f.data()) == RP_OK);
        CHECK(rp_runtime_rewind(rt) == RP_ERR_NOT_FOUND);  // ring size 1 (< 2)
        rp_runtime_unload_core(rt);
    }

    rp_runtime_destroy(rt);
}

// ---- Gated FCEUmm savestate e2e (real NES, deterministic) ----
//
// Same probe-and-WARN-skip gating as test_libretro_e2e.cpp / test_audio_flow.cpp: if the
// FCEUmm core DLL or a NES ROM is absent (e.g. CI, a clean checkout), skip cleanly rather
// than fail. On this machine (FCEUmm + Donkey Kong present under C:/RetroBat/roms/nes) it
// runs for real and is the slice's single provable claim against actual emulation: save at
// A, advance, load, and prove you are provably back at A -- pixel-exact.
TEST_CASE("FCEUmm savestate e2e: pixel-exact deterministic restore (gated)") {
    std::string rom = savestate_first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !savestate_file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll")) {
        WARN("no core/rom; skip");
        return;
    }
    const uint32_t W = 256, H = 240;   // NES resolution
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);

    // Advance past boot exactly like test_libretro_e2e.cpp: run until the displayed frame
    // actually changes rather than assuming a fixed boot-hold length for whichever ROM
    // happens to sort first on this machine.
    std::vector<uint8_t> early((size_t)W * H * 4, 0), boot((size_t)W * H * 4, 0);
    for (int i = 0; i < 10; i++) rp_runtime_present(rt, early.data());
    const int kMaxAdvance = 1000;
    for (int i = 0; i < kMaxAdvance; i++) {
        rp_runtime_present(rt, boot.data());
        if (boot != early) break;
    }
    REQUIRE(boot != early);   // genuinely past a static boot/title screen before we test

    size_t sz = rp_runtime_serialize_size(rt);
    REQUIRE(sz > 0);          // NES savestate is a few KB, never zero for a loaded core

    std::vector<uint8_t> state(sz);
    REQUIRE(rp_runtime_save_state(rt, state.data(), state.size()) == RP_OK);

    std::vector<uint8_t> r1((size_t)W * H * 4, 0);
    REQUIRE(rp_runtime_present(rt, r1.data()) == RP_OK);

    std::vector<uint8_t> advanced((size_t)W * H * 4, 0);
    for (int i = 0; i < 60; i++) REQUIRE(rp_runtime_present(rt, advanced.data()) == RP_OK);
    CHECK(advanced != r1);    // the game actually moved on since the save point

    REQUIRE(rp_runtime_load_state(rt, state.data(), state.size()) == RP_OK);

    std::vector<uint8_t> r2((size_t)W * H * 4, 0);
    REQUIRE(rp_runtime_present(rt, r2.data()) == RP_OK);
    CHECK(r2 == r1);          // re-executes deterministically from the restored state: pixel-exact

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
