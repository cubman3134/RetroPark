#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "audio/XAudio2Output.h"
#include <vector>
#include <string>
#include <filesystem>
#ifndef RP_SHIM_DIR
#define RP_SHIM_DIR "cores/libretro_shim"
#endif
#ifndef RP_NES_ROM_DIR
#define RP_NES_ROM_DIR "C:/RetroBat/roms/nes"
#endif

static bool audio_flow_file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string audio_flow_first_nes(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".nes") return entry.path().string();
    }
    return {};
}

TEST_CASE("audio flow: FCEUmm produces a plausible non-silent stereo stream") {
    std::string rom = audio_flow_first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !audio_flow_file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll")) {
        WARN("no core/rom; skip");
        return;
    }
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);   // audio path is backend-independent
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 256, 240) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);
    std::vector<uint8_t> img(256 * 240 * 4, 0);
    rp_input_state in{};
    uint64_t frames = 0; int nonsilent = 0;
    // Real NES titles commonly sit on a silent title/menu screen (sometimes more than
    // one, e.g. a team-select before kickoff) until Start advances them, so a plain
    // fixed-frame advance risks sampling only that silence regardless of which ROM
    // sorts first in RP_NES_ROM_DIR. Tap Start periodically and poll for sound, mirroring
    // test_libretro_e2e's bounded-advance-until-condition pattern rather than assuming a
    // fixed boot-hold length; kMinAudioFrames keeps an early nonsilent flip from being
    // reported on a too-short, sub-threshold sample.
    const int kMaxAdvance = 3000;
    const uint64_t kMinAudioFrames = 20000;
    for (int i = 0; i < kMaxAdvance; ++i) {
        in.keys[0x0D] = (i % 50 < 4) ? 1 : 0;   // VK_RETURN = Start, tapped briefly every ~50 frames
        rp_runtime_set_input(rt, &in);
        rp_runtime_present(rt, img.data());
        if (i % 20 == 0) {
            rp_runtime_audio_stats(rt, &frames, &nonsilent);
            if (nonsilent && frames > kMinAudioFrames) break;
        }
    }
    rp_runtime_audio_stats(rt, &frames, &nonsilent);
    // ~ sample_rate/fps stereo frames per video frame; NES ~ 48000/60 = 800/frame.
    CHECK(frames > 20000);           // clearly audio flowed (not zero / not a trickle)
    CHECK(nonsilent == 1);           // the game actually produced sound, not silence
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}

TEST_CASE("audio device: XAudio2 opens, plays a buffer, closes (gated)") {
    rp::XAudio2Output out; std::string err;
    if (out.open(48000, 2, err) != RP_OK) { WARN("no audio device; skip"); return; }
    std::vector<int16_t> tone(48000 * 2 / 10, 0);     // 0.1s of silence (submit path only)
    out.submit(tone.data(), tone.size() / 2);
    out.close();
    CHECK(true);                     // reaching here without crashing is the assertion
}
