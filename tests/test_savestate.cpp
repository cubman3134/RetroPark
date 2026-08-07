#include <doctest/doctest.h>
#include "loader/CoreLoader.h"
#include "loader/Win32CoreModule.h"
#include <retropark/retropark_abi.h>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

using namespace rp;

// Portable, device-free round-trip against the real refcore_driven DLL (loaded directly
// through CoreLoader + Win32CoreModule, not rp_runtime — the driven core needs no D3D11/
// Vulkan device, so this stays a plain unit test). The full pixel-exact e2e (advance,
// serialize, advance further, unserialize, re-render, compare frames) is Task 3.
namespace {
void noop_log(rp_host*, int, const char*) {}
void noop_submit(rp_host*, uint32_t, uint64_t, uint64_t) {}
void noop_input(rp_host*, rp_input_state* out) { *out = rp_input_state{}; }
void noop_video(rp_host*, const void*, uint32_t, uint32_t, uint32_t) {}
void noop_audio(rp_host*, const int16_t*, size_t) {}

rp_result open_driven(std::unique_ptr<Win32CoreModule>& mod, CoreLoader& ld, std::string& err) {
    rp_result r = Win32CoreModule::open(std::string(RP_DRIVEN_CORE_DIR) + "/refcore_driven.dll", mod, err);
    if (r != RP_OK) return r;
    r = ld.load(mod.get(), err);
    if (r != RP_OK) return r;
    rp_host_iface host{};
    host.log = noop_log;
    host.submit_frame = noop_submit;
    host.input_state = noop_input;
    host.video_refresh = noop_video;
    host.audio_sample = noop_audio;
    return ld.create(&host, err);
}
}

TEST_CASE("refcore_driven: serialize_size is 4 bytes") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);
    CHECK(ld.serialize_size() == 4u);
    ld.destroy();
}

TEST_CASE("refcore_driven: serialize/unserialize round-trips the frame counter") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);

    for (int i = 0; i < 5; ++i) REQUIRE(ld.run_frame(err) == RP_OK);

    std::vector<uint8_t> snap(ld.serialize_size());
    REQUIRE(ld.serialize(snap.data(), snap.size(), err) == RP_OK);

    for (int i = 0; i < 10; ++i) REQUIRE(ld.run_frame(err) == RP_OK);

    std::vector<uint8_t> advanced(ld.serialize_size());
    REQUIRE(ld.serialize(advanced.data(), advanced.size(), err) == RP_OK);
    CHECK(advanced != snap);   // state actually moved on

    REQUIRE(ld.unserialize(snap.data(), snap.size(), err) == RP_OK);

    std::vector<uint8_t> restored(ld.serialize_size());
    REQUIRE(ld.serialize(restored.data(), restored.size(), err) == RP_OK);
    CHECK(restored == snap);   // back to the captured counter

    ld.destroy();
}

TEST_CASE("refcore_driven: serialize rejects undersized buffer / null data") {
    std::unique_ptr<Win32CoreModule> mod; CoreLoader ld; std::string err;
    REQUIRE(open_driven(mod, ld, err) == RP_OK);

    uint8_t buf[4] = {0};
    CHECK(ld.serialize(buf, 3, err) == RP_ERR_BAD_ARG);
    CHECK(ld.serialize(nullptr, 4, err) == RP_ERR_BAD_ARG);
    CHECK(ld.unserialize(buf, 3, err) == RP_ERR_BAD_ARG);
    CHECK(ld.unserialize(nullptr, 4, err) == RP_ERR_BAD_ARG);

    ld.destroy();
}
