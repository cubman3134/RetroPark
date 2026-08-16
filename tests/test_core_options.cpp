#include <doctest/doctest.h>
#include "retropark/retropark.h"
#include <cstring>

// The driven ref core dir is provided target-wide by tests/CMakeLists.txt (RP_DRIVEN_CORE_DIR ->
// build/cores/refcore_driven). Fall back to a relative path if built standalone, mirroring the
// other driven-core e2e tests.
#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
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
