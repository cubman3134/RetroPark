#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include "render/gl/GLBackend.h"
#include <vector>
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
static void run_driven(rp_graphics_api api) {
    const uint32_t W=64,H=64;
    rp_runtime* rt = rp_runtime_create(api, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // driven core loads on ANY api
    std::vector<uint8_t> img((size_t)W*H*4, 0);
    bool sawGreen=false;
    for (int i=0;i<10 && !sawGreen;i++) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        if (img[(((size_t)(H-4))*W + (W-4))*4 + 1] > 150) sawGreen = true;
    }
    CHECK(sawGreen);   // the driven core frame composites into our surface
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("driven e2e: D3D11") { run_driven(RP_GFX_D3D11); }
TEST_CASE("driven e2e: Vulkan") {
    if (!rp::VulkanBackend::probe_vulkan_shared()) { WARN("no vulkan"); return; }
    run_driven(RP_GFX_VULKAN);
}
TEST_CASE("driven e2e: OpenGL") {
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no OpenGL 3.3"); return; }
    run_driven(RP_GFX_OPENGL);                                   // green shows in the BR quadrant readback
}
TEST_CASE("driven e2e: resize a loaded core (OpenGL)") {
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no OpenGL 3.3"); return; }
    rp_runtime* rt=rp_runtime_create(RP_GFX_OPENGL,nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt,64,64)==RP_OK);
    REQUIRE(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);
    CHECK(rp_runtime_resize(rt,96,72)==RP_OK);                   // resize path (rebuild_surfaces) must not fail
    std::vector<uint8_t> img(96*72*4,0); bool green=false;
    for(int i=0;i<10&&!green;i++){ if(rp_runtime_present(rt,img.data())==RP_OK &&
        img[(((size_t)(72-4))*96+(96-4))*4+1]>150) green=true; }
    CHECK(green);
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("driven e2e: reload + not-bricked (D3D11)") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr); REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt,64,64)==RP_OK);
    CHECK(rp_runtime_load_core(rt,"no_such_dir")==RP_ERR_NOT_FOUND);
    CHECK(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);       // recovers
    CHECK(rp_runtime_load_core(rt,RP_DRIVEN_CORE_DIR)==RP_OK);       // reload while loaded
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
