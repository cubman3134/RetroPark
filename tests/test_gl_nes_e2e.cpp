#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/gl/GLBackend.h"
#include <vector>
#include <string>
#include <filesystem>
#include <cstdlib>
#ifndef RP_SHIM_DIR
#define RP_SHIM_DIR "cores/libretro_shim"
#endif
#ifndef RP_NES_ROM_DIR
#define RP_NES_ROM_DIR "C:/RetroBat/roms/nes"
#endif

static bool gl_nes_file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string gl_nes_first_nes(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".nes") return entry.path().string();
    }
    return {};
}

// Real NES (software libretro shim + FCEUmm) composited through the OpenGL host backend.
// Opt-in only (needs a real GPU + real NES ROM). Set RP_RUN_GL_NES=1 to run.
TEST_CASE("libretro e2e: OpenGL runs a real NES ROM") {
    if (!std::getenv("RP_RUN_GL_NES")) { WARN("RP_RUN_GL_NES not set; skipping GL NES e2e"); return; }
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no capable OpenGL 3.3; skipping"); return; }

    std::string rom = gl_nes_first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !gl_nes_file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll")) {
        WARN("no core/rom; skip");
        return;
    }
    const uint32_t W = 256, H = 240;   // NES resolution
    rp_runtime* rt = rp_runtime_create(RP_GFX_OPENGL, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);
    std::vector<uint8_t> early((size_t)W * H * 4, 0), late((size_t)W * H * 4, 0);
    for (int i = 0; i < 10; i++) rp_runtime_present(rt, early.data());   // warm up + early frame
    // Advance in a bounded loop and stop as soon as the frame changes, so the test proves
    // genuine emulation progress rather than assuming a specific boot-hold length; a ROM that
    // never changes within the budget still fails. (Mirrors the D3D11/Vulkan NES e2e.)
    const int kMaxAdvance = 1000;
    for (int i = 0; i < kMaxAdvance; i++) {
        rp_runtime_present(rt, late.data());
        if (late != early) break;
    }
    // (1) not near-black: some pixel is meaningfully bright
    uint64_t sum = 0;
    for (uint8_t v : late) sum += v;
    CHECK(sum > (uint64_t)W * H);      // not an all-black frame
    // (2) changed across frames: early != late
    CHECK(early != late);
    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
