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
#ifndef RP_N64_ROM
#define RP_N64_ROM "C:/Users/cubma/AppData/Local/Temp/n64rom/Banjo-Tooie (USA).n64"
#endif

// REPRO for the in-app "N64 pause-menu Core Options won't come up" bug: EB queries
// rp_runtime_core_options_json on the RUNNING core (AFTER load_content), but every existing test only
// checks it after load_core (no content). Mimic the in-app sequence and assert the options SURVIVE
// content load (the HW-render N64 path re-enters retro_load_game, which some cores use to re-declare
// options). Gated like the N64 e2e; needs the extracted ROM (RP_N64_ROM env overrides).
TEST_CASE("core options: N64 options survive content load (in-app sequence)") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping N64 content-load options repro"); return; }
    const char* rom = std::getenv("RP_N64_ROM") ? std::getenv("RP_N64_ROM") : RP_N64_ROM;
    // RP_TEST_GL selects the OpenGL host runtime (B2 zero-copy N64 path) vs the default D3D11 readback path,
    // so this reproduces whichever driven backend the user picked.
    const rp_graphics_api api = std::getenv("RP_TEST_GL") ? RP_GFX_OPENGL : RP_GFX_D3D11;
    rp_runtime* rt = rp_runtime_create(api, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_resize(rt, 640, 480) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);
    const std::string beforeContent = rp_runtime_core_options_json(rt);
    MESSAGE("after load_core: " << beforeContent.substr(0, 80));
    CHECK(beforeContent.find("mupen64plus") != std::string::npos);   // present after load_core (proven by A2)

    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);               // HW-render setup + retro_load_game
    const std::string afterContent = rp_runtime_core_options_json(rt);
    MESSAGE("after load_content: " << afterContent.substr(0, 80));
    CHECK(afterContent.find("mupen64plus") != std::string::npos);     // <-- do options SURVIVE content load?
    rp_runtime_destroy(rt);
}

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

// Extract the substring value of the first occurrence of "\"field\":\"...\"" at or after `from`,
// advancing `from` past the closing quote. Returns "" and leaves from unchanged if not found.
// Intentionally minimal (no escape handling): the mupen option keys/values are plain ASCII.
static std::string json_first_string(const std::string& j, const char* field, size_t& from) {
    std::string needle = std::string("\"") + field + "\":\"";
    size_t k = j.find(needle, from);
    if (k == std::string::npos) return {};
    size_t start = k + needle.size();
    size_t end = j.find('"', start);
    if (end == std::string::npos) return {};
    from = end + 1;
    return j.substr(start, end - start);
}

// A3: set an override, and verify get echoes it (override wins over default) and an unknown key is
// rejected. Gated like the harvest case above; uses D3D11 (RP_GFX_NONE builds no backend -> load_core
// RP_ERR_DEVICE). Keys/values are read out of the harvested JSON so the test never asserts on a key
// or value this mupen build didn't actually produce.
TEST_CASE("core options: set override is echoed by get and flagged to the core") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping shim core-options override"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);

    // Pull the first option key + its default out of the harvested JSON, then find any value[] entry
    // that differs from the default -- that is the override we will apply.
    std::string json = rp_runtime_core_options_json(rt);
    size_t cur = 0;
    std::string key = json_first_string(json, "key", cur);
    REQUIRE(key.size() > 0);
    std::string def = json_first_string(json, "default", cur);
    // Values array of THIS first option follows immediately after its "default" field.
    std::string newv;
    for (size_t vc = cur;;) {
        std::string v = json_first_string(json, "value", vc);
        if (v.empty()) break;
        if (v != def) { newv = v; break; }
    }
    REQUIRE(newv.size() > 0);   // this option must expose at least one non-default value

    const char* before = rp_runtime_core_option_get(rt, key.c_str());
    REQUIRE(before != nullptr);
    CHECK(std::strcmp(before, def.c_str()) == 0);              // get returns the default before any set
    CHECK(rp_runtime_core_option_set(rt, key.c_str(), newv.c_str()) == RP_OK);
    CHECK(std::strcmp(rp_runtime_core_option_get(rt, key.c_str()), newv.c_str()) == 0);  // override echoed
    CHECK(rp_runtime_core_option_set(rt, "no-such-key", "x") == RP_ERR_NOT_FOUND);
    rp_runtime_destroy(rt);
}

// A3 lifetime regression: rp_runtime_core_option_get returns storage owned by the core that the header
// documents as valid until the next core unload, so a pointer held for one key must survive a get of a
// DIFFERENT key. Before the per-key served_values fix, every get aliased a single shared buffer, so the
// first pointer would start reading the second key's value (or dangle). Gated like the cases above.
TEST_CASE("core options: a held get survives a get of another key") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping shim core-options lifetime"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt != nullptr);
    REQUIRE(rp_runtime_load_core(rt, RP_N64_SHIM_DIR) == RP_OK);

    // Two distinct harvested keys (mupen exposes many).
    std::string json = rp_runtime_core_options_json(rt);
    size_t cur = 0;
    std::string keyA = json_first_string(json, "key", cur);
    std::string keyB = json_first_string(json, "key", cur);
    REQUIRE(keyA.size() > 0);
    REQUIRE(keyB.size() > 0);
    REQUIRE(keyA != keyB);

    const char* a = rp_runtime_core_option_get(rt, keyA.c_str());
    REQUIRE(a != nullptr);
    std::string a_when_fetched = a;                       // what keyA read the instant we got it
    const char* b = rp_runtime_core_option_get(rt, keyB.c_str());
    REQUIRE(b != nullptr);
    CHECK(a != b);                                        // distinct per-key backing storage
    CHECK(std::strcmp(a, a_when_fetched.c_str()) == 0);   // a still reads keyA's value, not keyB's
    rp_runtime_destroy(rt);
}
