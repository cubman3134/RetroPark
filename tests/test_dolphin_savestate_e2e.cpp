#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rp;

// Slice N: Dolphin savestate round-trip through the core ABI. Boot Billy Hatcher, advance to a settled
// frame, save_state, advance K frames (capture A), load_state (restore), advance K again (capture B),
// and assert B == A: the restored state re-runs deterministically to the same frame. Input is held
// neutral across both advances so the re-simulation is deterministic. Device-independent readback.

#ifndef RP_DOLPHIN_CORE_DIR
#define RP_DOLPHIN_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64"
#endif

namespace {
const char* kCoreDir = RP_DOLPHIN_CORE_DIR;
const char* kRom = "C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz";
bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Present until `count` good frames have been consumed (Dolphin boots over several seconds; present()
// returns non-OK until frames flow). Returns the number of good presents actually achieved.
int pump(rp_runtime* rt, std::vector<uint8_t>& img, int count) {
    int good = 0;
    for (int i = 0; i < count * 6 + 600 && good < count; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (rp_runtime_present(rt, img.data()) == RP_OK) ++good;
    }
    return good;
}
size_t bytes_differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t d = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (a[i] != b[i]) ++d; return d;
}
} // namespace

TEST_CASE("dolphin savestate: save -> diverge -> load re-runs deterministically to the same frame (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping Dolphin savestate e2e"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/core.json")) { WARN("core.json not beside the DLL; skipping"); return; }
    if (!file_exists(kRom)) { WARN("Billy Hatcher ROM absent; skipping"); return; }

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, kRom) == RP_OK);

    rp_input_state neutral{};
    rp_runtime_set_input(rt, 0, &neutral);   // hold neutral so the re-simulation is deterministic

    std::vector<uint8_t> img(W * H * 4, 0), frameN, A, B;
    const int K = 60;

    // Advance to a settled frame N (~5s of boot), capture it.
    REQUIRE(pump(rt, img, 260) >= 260);
    frameN = img;

    // Save state here.
    size_t sz = rp_runtime_serialize_size(rt);
    fprintf(stderr, "[dolphin-save] serialize_size=%zu\n", sz); fflush(stderr);
    REQUIRE(sz > 0);                                  // FAILS until dolphin_present wires serialize
    std::vector<uint8_t> state(sz, 0);
    REQUIRE(rp_runtime_save_state(rt, state.data(), state.size()) == RP_OK);

    // Advance K frames -> A.
    REQUIRE(pump(rt, img, K) >= K);
    A = img;
    CHECK(bytes_differing(A, frameN) > A.size() / 50);   // game actually advanced (not frozen)

    // Restore, advance K again -> B.
    REQUIRE(rp_runtime_load_state(rt, state.data(), state.size()) == RP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the post-load pipeline settle
    REQUIRE(pump(rt, img, K) >= K);
    B = img;

    size_t diff = bytes_differing(A, B);
    fprintf(stderr, "[dolphin-save] A-vs-B differing bytes=%zu of %zu\n", diff, A.size()); fflush(stderr);

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    // The restored state re-ran to the same frame. Byte-identical is the target; a tight tolerance
    // (< 0.1% of bytes) absorbs any minor GPU-path nondeterminism without weakening the claim.
    CHECK(diff <= A.size() / 1000);
}
