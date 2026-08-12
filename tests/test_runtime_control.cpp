#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include <vector>
#ifndef RP_VK_CORE_DIR
#define RP_VK_CORE_DIR "."
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
