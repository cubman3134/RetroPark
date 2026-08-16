#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rp;

// Task A1: the Dolphin presenting vehicle exposes internal-resolution + aspect-ratio as core options
// through rp_core_abi's core_options_json / core_option_get / core_option_set slots (v9). The Runtime
// forwards them via rp_runtime_core_options_json / _get / _set (CoreLoader -> ABI). This gated test drives
// that path against the real dolphin_present.dll: after rp_runtime_load_core the loader is in the Created
// state, so the option quartet is reachable WITHOUT booting the (multi-minute) GC game. We assert the JSON
// descriptor advertises both keys, that set/get round-trips an override, and that an unknown key is
// rejected with RP_ERR_NOT_FOUND. The boot-time apply (g_dp_options -> Config::GFX_EFB_SCALE) is exercised
// by the vehicle's pre-boot config loop; the harness does not link Dolphin's Config so it cannot read
// GFX_EFB_SCALE back directly — the set/get/json contract is the observable ABI proof here.

#ifndef RP_DOLPHIN_CORE_DIR
#define RP_DOLPHIN_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64"
#endif

namespace {
const char* kCoreDir = RP_DOLPHIN_CORE_DIR;

bool file_exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
} // namespace

TEST_CASE("dolphin core: internal-resolution + aspect-ratio exposed as core options (gated)") {
    // Opt-in only (needs the built dolphin_present vehicle). Set RP_RUN_DOLPHIN=1 to run.
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping Dolphin core-options test"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/core.json")) { WARN("dolphin_present core.json not present beside the DLL; skipping"); return; }

    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 640, 480) == RP_OK);

    // Loading the core leaves the presenting loader in Created (start is deferred until content) — the
    // core-options quartet is reachable in that state, no boot required.
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);

    // JSON descriptor advertises both option keys.
    const char* json = rp_runtime_core_options_json(rt);
    REQUIRE(json != nullptr);
    fprintf(stderr, "[dolphin-opts] options json: %s\n", json); fflush(stderr);
    CHECK(std::strstr(json, "dolphin_internal_resolution") != nullptr);
    CHECK(std::strstr(json, "dolphin_aspect_ratio") != nullptr);

    // Defaults before any override.
    const char* res_def = rp_runtime_core_option_get(rt, "dolphin_internal_resolution");
    REQUIRE(res_def != nullptr);
    CHECK(std::strcmp(res_def, "1") == 0);
    const char* ar_def = rp_runtime_core_option_get(rt, "dolphin_aspect_ratio");
    REQUIRE(ar_def != nullptr);
    CHECK(std::strcmp(ar_def, "0") == 0);

    // Set an override (pre-boot: the vehicle stashes it, the boot config block applies it to GFX_EFB_SCALE)
    // and confirm get echoes it back through the Runtime/CoreLoader/ABI path.
    CHECK(rp_runtime_core_option_set(rt, "dolphin_internal_resolution", "2") == RP_OK);
    const char* res_now = rp_runtime_core_option_get(rt, "dolphin_internal_resolution");
    REQUIRE(res_now != nullptr);
    CHECK(std::strcmp(res_now, "2") == 0);

    // Aspect ratio round-trips too.
    CHECK(rp_runtime_core_option_set(rt, "dolphin_aspect_ratio", "1") == RP_OK);
    const char* ar_now = rp_runtime_core_option_get(rt, "dolphin_aspect_ratio");
    REQUIRE(ar_now != nullptr);
    CHECK(std::strcmp(ar_now, "1") == 0);

    // Unknown key: set is rejected, get returns null.
    CHECK(rp_runtime_core_option_set(rt, "dolphin_bogus_key", "1") == RP_ERR_NOT_FOUND);
    CHECK(rp_runtime_core_option_get(rt, "dolphin_bogus_key") == nullptr);

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
}
