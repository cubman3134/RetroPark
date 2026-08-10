#include <doctest/doctest.h>
#include "loader/StaticCoreModule.h"
#include "loader/StaticCoreRegistry.h"
#include "loader/CoreLoader.h"
#include <retropark/retropark_abi.h>
#include <retropark/retropark.h>
#include <cstdint>
#include <string>
#include <vector>

using namespace rp;

// Two static cores compiled into the test binary (RefCoreDriven.cpp, twice, with the getter renamed per
// copy — see tests/CMakeLists.txt). Declared here; registered by register_static_test_cores().
extern "C" const rp_core_abi* refcore_driven_static_get_core_abi(void);
extern "C" const rp_core_abi* refcore_driven_b_static_get_core_abi(void);

static void register_static_test_cores() {
    StaticCoreRegistry::register_core("refcore_driven", &refcore_driven_static_get_core_abi);
    StaticCoreRegistry::register_core("refcore_driven_b", &refcore_driven_b_static_get_core_abi);
}

// refcore_driven derefs its host in create() and calls host.video_refresh() every run_frame(), so the
// device-independent proof still needs a valid host — just one with a no-op sink (no GPU, no Runtime).
static void noop_video_refresh(rp_host*, const void*, uint32_t, uint32_t, uint32_t) {}

TEST_CASE("static core: registry resolves registered ids, rejects unknown") {
    register_static_test_cores();
    CHECK(StaticCoreRegistry::has("refcore_driven"));
    CHECK(StaticCoreRegistry::has("refcore_driven_b"));   // second static core links -> no symbol collision
    CHECK_FALSE(StaticCoreRegistry::has("nope"));
    CHECK(StaticCoreRegistry::get("refcore_driven") != nullptr);
    CHECK(StaticCoreRegistry::get("nope") == nullptr);
}

TEST_CASE("static core: loads + runs through CoreLoader with no DLL and no GPU") {
    register_static_test_cores();
    StaticCoreModule mod(StaticCoreRegistry::get("refcore_driven"));
    CoreLoader ld; std::string err;
    REQUIRE(ld.load(&mod, err) == RP_OK);                 // resolves the compiled-in getter, checks abi_version
    rp_host_iface host{}; host.video_refresh = &noop_video_refresh;
    REQUIRE(ld.create(&host, err) == RP_OK);
    // Drive it: refcore_driven's serialized state is its frame counter (Slice F). Advance -> it changes.
    for (int i = 0; i < 5; ++i) REQUIRE(ld.run_frame(err) == RP_OK);
    std::vector<uint8_t> a(ld.serialize_size());
    REQUIRE(ld.serialize(a.data(), a.size(), err) == RP_OK);
    for (int i = 0; i < 7; ++i) REQUIRE(ld.run_frame(err) == RP_OK);
    std::vector<uint8_t> b(ld.serialize_size());
    REQUIRE(ld.serialize(b.data(), b.size(), err) == RP_OK);
    CHECK(a != b);                                        // the statically-linked core actually ran
    ld.destroy();
}

TEST_CASE("static core: Runtime loads it by id and drives frames through present (no DLL)") {
    register_static_test_cores();
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);   // headless WARP, like the driven e2e
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    CHECK(rp_runtime_load_static_core(rt, "no_such_id") == RP_ERR_NOT_FOUND);
    REQUIRE(rp_runtime_load_static_core(rt, "refcore_driven") == RP_OK);   // loaded from the registry, no DLL
    std::vector<uint8_t> img(64 * 64 * 4, 0);
    bool sawGreen = false;
    for (int i = 0; i < 60 && !sawGreen; ++i) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        if (img[((64 - 4) * 64 + (64 - 4)) * 4 + 1] > 150) sawGreen = true;  // core's green (driven e2e check)
    }
    CHECK(sawGreen);
    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
