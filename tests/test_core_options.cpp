#include <doctest/doctest.h>
#include "retropark/retropark.h"
#include <cstring>
#include <cstdlib>
#include <string>

// The driven ref core dir is provided target-wide by tests/CMakeLists.txt (RP_DRIVEN_CORE_DIR ->
// build/cores/refcore_driven). Fall back to a relative path if built standalone, mirroring the
// other driven-core e2e tests.
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

// The N64 shim package dir (LibretroShim.dll + core.json => mupen64plus_next). Provided target-wide
// by tests/CMakeLists.txt (RP_N64_SHIM_DIR -> build/cores/libretro_shim_n64); fall back to the same
// literal the N64 HW-render e2e uses when built standalone.
#ifndef RP_N64_SHIM_DIR
#define RP_N64_SHIM_DIR "C:/Users/cubma/source/repos/RetroPark/build/cores/libretro_shim_n64"
#endif

// A driven core with no option channel (fptrs NULL) must degrade gracefully through the whole
// runtime -> loader -> core forwarding chain: json "[]", get NULL, set RP_ERR_UNSUPPORTED.
TEST_CASE("core options: no-options core degrades gracefully") {
    // A real backend is needed for load_core (RP_GFX_NONE has no backend); the driven ref core
    // loads on any api, and the option-channel degradation is backend-independent.
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    // refcore_driven has no options; load it from its build dir.
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);

    const char* json = rp_runtime_core_options_json(rt);
    REQUIRE(json != nullptr);
    CHECK(std::strcmp(json, "[]") == 0);
    CHECK(rp_runtime_core_option_get(rt, "anything") == nullptr);
    CHECK(rp_runtime_core_option_set(rt, "anything", "1") == RP_ERR_UNSUPPORTED);

    rp_runtime_destroy(rt);
}

// Gate exactly like the N64 e2e: only runs when RP_RUN_N64 is set; uses the same shim core dir + ROM.
// A real backend is needed for load_core (RP_GFX_NONE has no backend -> load_core RP_ERR_DEVICE); the
// shim harvests its wrapped core's options at create/set_environment time, before any HW-render/content
// setup, so the harvest is backend-independent -- D3D11 (like the no-options case above) is fine here.
TEST_CASE("core options: shim harvests real core options as JSON") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping shim core-options harvest"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);  // build/cores/libretro_shim_n64

    std::string json = rp_runtime_core_options_json(rt);
    CHECK(json.size() > 2);                 // not "[]"
    CHECK(json.front() == '[');
    CHECK(json.find("\"key\"") != std::string::npos);
    CHECK(json.find("mupen64plus") != std::string::npos);   // mupen option keys are prefixed mupen64plus-*
    rp_runtime_destroy(rt);
}
