#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include <vector>
#include <thread>
#include <chrono>
#ifndef RP_VK_CORE_DIR
#define RP_VK_CORE_DIR "."
#endif
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

TEST_CASE("runtime control: get_status reflects core type + pause flag") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    rp_runtime_status st{};
    // no core yet
    CHECK(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.content_loaded == 0);
    CHECK(rp_runtime_get_status(rt, nullptr) == RP_ERR_BAD_ARG);
    // load the reference presenting core (content-free: it renders without load_content)
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.core_type == RP_CORE_PRESENTING);
    CHECK(st.graphics_api == RP_GFX_VULKAN);
    CHECK(st.paused == 0);
    CHECK(rp_runtime_pause(rt) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.paused == 1);
    CHECK(rp_runtime_resume(rt) == RP_OK);
    REQUIRE(rp_runtime_get_status(rt, &st) == RP_OK);
    CHECK(st.paused == 0);
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime control: driven pause freezes the frame") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // content-free driven ref core
    std::vector<uint8_t> a(64*64*4), b(64*64*4), c(64*64*4);
    REQUIRE(rp_runtime_present(rt, a.data()) == RP_OK);
    REQUIRE(rp_runtime_present(rt, b.data()) == RP_OK);
    CHECK(a != b);                                   // animates while running
    REQUIRE(rp_runtime_pause(rt) == RP_OK);
    REQUIRE(rp_runtime_present(rt, a.data()) == RP_OK);
    REQUIRE(rp_runtime_present(rt, b.data()) == RP_OK);
    CHECK(a == b);                                   // frozen while paused
    REQUIRE(rp_runtime_resume(rt) == RP_OK);
    REQUIRE(rp_runtime_present(rt, c.data()) == RP_OK);
    CHECK(c != b);                                   // resumes
    rp_runtime_destroy(rt);
}

TEST_CASE("runtime control: presenting pause freezes the frame") {
    using namespace std::chrono_literals;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_VK_CORE_DIR) == RP_OK);   // refcore_present_vk: rising-blue animation
    std::vector<uint8_t> a(64*64*4), b(64*64*4);
    // Liveness: the presenting core animates on its own producer thread (a new frame
    // ~every 16ms). Two presents a couple of frame-times apart differ while running,
    // which makes the freeze assertion below meaningful (a dead core would pass trivially).
    rp_runtime_present(rt, a.data());
    std::this_thread::sleep_for(40ms);
    rp_runtime_present(rt, b.data());
    REQUIRE(a != b);                              // animates while running
    // Cache a freshly-produced frame immediately before pausing, so the whole 3-slot
    // ring cycle (~48ms) of headroom remains before the producer recycles that slot.
    std::this_thread::sleep_for(20ms);
    rp_runtime_present(rt, a.data());             // this (unpaused) present caches "last ready"
    REQUIRE(rp_runtime_pause(rt) == RP_OK);
    // While paused the consumer re-presents the cached ring frame and IGNORES newly
    // submitted ones. The two grabs straddle ~2 producer frames (28ms): unpaused,
    // latest_ready would have advanced and they'd differ; paused, they are byte-identical.
    rp_runtime_present(rt, a.data());
    std::this_thread::sleep_for(28ms);
    rp_runtime_present(rt, b.data());
    CHECK(a == b);                                // frozen while paused: cached frame re-presented
    rp_runtime_destroy(rt);
}
