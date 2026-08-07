#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <vector>
#include <thread>
#include <chrono>
#include <string>

using namespace rp;

#ifndef RP_VK_CORE_DIR
#define RP_VK_CORE_DIR "cores/refcore_present_vk"   // relative to test CWD; overridden by CMake
#endif

namespace {

// Poll present() for up to ~60 frames (~16ms apiece), filling img and returning
// once the core's green shows up in the bottom-right quadrant. Returns the last
// rp_result from present() and whether green was observed via sawCore.
rp_result pump_until_green(rp_runtime* rt, uint32_t W, uint32_t H, std::vector<uint8_t>& img, bool& sawCore) {
    rp_result pr = RP_ERR_INTERNAL;
    sawCore = false;
    for (int i = 0; i < 60 && !sawCore; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        pr = rp_runtime_present(rt, img.data());
        if (pr != RP_OK) continue;
        uint8_t g = img[((H - 4) * W + (W - 4)) * 4 + 1];
        if (g > 150) sawCore = true;
    }
    return pr;
}

} // namespace

TEST_CASE("vk e2e: reference Vulkan core renders into our surface and we composite an overlay") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device"); return; }
    const uint32_t W=64, H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);

    // Give the core thread a few frames to submit.
    std::vector<uint8_t> img(W*H*4, 0);
    rp_result pr = RP_ERR_INTERNAL;
    bool sawCore = false;
    for (int i = 0; i < 60 && !sawCore; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        pr = rp_runtime_present(rt, img.data());
        if (pr != RP_OK) continue;
        // Bottom-right quadrant should carry the core's green once a frame lands.
        uint8_t g = img[((H-4)*W + (W-4))*4 + 1];
        if (g > 150) sawCore = true;
    }
    CHECK(pr == RP_OK);
    CHECK(sawCore);

    auto at=[&](uint32_t x,uint32_t y,int c){ return img[(y*W+x)*4+c]; };
    // Overlay quadrant (top-left) shows a blue-ward blend over the core.
    CHECK(at(4,4,2) > 80);
    CHECK(at(4,4,1) < at(W-4,H-4,1));

    // Present several times back-to-back with no sleep: the host outruns the core, so
    // most (usually all) of these re-composite the same producer frame. The stale-frame
    // guard must skip the QFOT re-acquire and the timeline re-signal on a repeat (a
    // re-signal to an already-reached value is a validation error) while STILL handing
    // back valid pixels every call — the core's green must remain in the readback.
    // Present several times back-to-back with no sleep. When the host outruns the core,
    // latest_ready() hands back a producer value already consumed; the stale-frame guard
    // must then skip the QFOT re-acquire and the timeline re-signal (re-signalling a
    // reached value is illegal) while STILL re-compositing and reading back valid pixels
    // — the core's green must survive every call. (Present cadence here is close to the
    // core's ~16ms, so a true repeat is not guaranteed to occur; the guard's correctness
    // mirrors the reviewed windowed guard, and this asserts the no-regression envelope.)
    for (int i = 0; i < 8; ++i) {
        rp_result rr = rp_runtime_present(rt, img.data());
        CHECK(rr == RP_OK);
        CHECK(img[((H-4)*W + (W-4))*4 + 1] > 150);
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}

TEST_CASE("vk e2e: reloading the core on a live runtime cleanly tears down and restarts") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device"); return; }
    const uint32_t W=64, H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);

    std::vector<uint8_t> img(W*H*4, 0);
    bool sawCore = false;
    rp_result pr = pump_until_green(rt, W, H, img, sawCore);
    CHECK(pr == RP_OK);
    CHECK(sawCore);

    // Load again while a core is already running: must cleanly stop the old
    // instance (joining its thread and freeing its DLL only after it's stopped)
    // and start a fresh one, rather than crashing or leaking the old thread.
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);

    std::vector<uint8_t> img2(W*H*4, 0);
    bool sawCore2 = false;
    rp_result pr2 = pump_until_green(rt, W, H, img2, sawCore2);
    CHECK(pr2 == RP_OK);
    CHECK(sawCore2);

    auto at2=[&](uint32_t x,uint32_t y,int c){ return img2[(y*W+x)*4+c]; };
    CHECK(at2(4,4,2) > 80);
    CHECK(at2(4,4,1) < at2(W-4,H-4,1));

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}

TEST_CASE("vk e2e: a failed load does not brick the runtime for a real subsequent load") {
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device"); return; }
    const uint32_t W=64, H=64;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);

    CHECK(rp_runtime_load_core(rt, "no_such_dir") == RP_ERR_NOT_FOUND);

    // The runtime must still be usable: a real core can load and run afterward.
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);

    std::vector<uint8_t> img(W*H*4, 0);
    bool sawCore = false;
    rp_result pr = pump_until_green(rt, W, H, img, sawCore);
    CHECK(pr == RP_OK);
    CHECK(sawCore);

    auto at=[&](uint32_t x,uint32_t y,int c){ return img[(y*W+x)*4+c]; };
    CHECK(at(4,4,2) > 80);
    CHECK(at(4,4,1) < at(W-4,H-4,1));

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
