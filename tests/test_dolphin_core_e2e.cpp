#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rp;

// Slice K: dolphin_present is a reusable RetroPark presenting core. The RUNTIME loads it exactly like
// refcore_present_vk — rp_runtime_load_core(cores/dolphin_present) + rp_runtime_load_content(any GC ISO)
// — and drives it through rp_runtime_present. Dolphin (built from source, no libretro) renders the real
// Billy Hatcher ROM into RetroPark's exported shared VkImage (copy-from-XFB), and the Runtime composites
// our overlay on top — all through rp_core_abi, no test-only C API and no manual consume loop. This is
// the whole reusable-core payoff of the Dolphin arc, proven end to end through the public API.

#ifndef RP_DOLPHIN_CORE_DIR
#define RP_DOLPHIN_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64"
#endif

namespace {
const char* kCoreDir = RP_DOLPHIN_CORE_DIR;
const char* kRom = "C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz";

bool file_exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "real content" = a meaningful spread of non-dark pixels with real dynamic range (not black/uniform).
bool looks_rendered(const std::vector<uint8_t>& img) {
    size_t bright = 0; int mn = 255, mx = 0;
    for (size_t i = 0; i < img.size(); i += 4) {
        int v = (img[i] + img[i + 1] + img[i + 2]) / 3;
        if (v > 24) ++bright;
        mn = v < mn ? v : mn; mx = v > mx ? v : mx;
    }
    return bright > (img.size() / 4) / 20 && (mx - mn) > 40;
}
} // namespace

TEST_CASE("dolphin core: Runtime loads dolphin_present + a GC ISO and presents it with an overlay (gated)") {
    // Opt-in only (heavy: builds a real GC frame pipeline). Set RP_RUN_DOLPHIN=1 to run.
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping Dolphin core e2e"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/core.json")) { WARN("dolphin_present core.json not present beside the DLL; skipping"); return; }
    if (!file_exists(kRom)) { WARN("Billy Hatcher ROM absent; skipping"); return; }

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);

    // Load the core (defers start — dolphin_present is a presenting core that requires content) then
    // hand it the ISO (this is what triggers the boot).
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, kRom) == RP_OK);

    // Dolphin boots over a few seconds; present() returns non-OK until the first XFB frame lands in the
    // ring. Poll for the first good frame, then keep pumping to capture an early and a late frame.
    // The video/overlay proof is captured by frame 240 (the settled early-boot frame). Dolphin's boot
    // logos are SILENT for the first ~13 game-seconds, though, and the lock-step present throttles
    // emulation to the host's consume rate (~1 game-frame per present), so audio does not begin until
    // several hundred more frames in. After the video proof is captured we therefore keep pumping
    // present() (advancing the emulation) until the device-independent audio counters go non-silent,
    // bounded by a hard frame cap so a genuinely silent core still terminates and fails.
    std::vector<uint8_t> img(W * H * 4, 0), early, late;
    int good = 0;
    fprintf(stderr, "[dolphin-core] booting via Runtime; polling present()...\n"); fflush(stderr);
    for (int i = 0; i < 3000; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        rp_result pr = rp_runtime_present(rt, img.data());
        if (pr != RP_OK) continue;
        ++good;
        if (good == 30) early = img;               // a settled early frame
        if (good <= 240) late = img;               // freeze the late frame at the settled boot screen
        if (good % 60 == 0) { fprintf(stderr, "[dolphin-core] %d frames presented\n", good); fflush(stderr); }
        if (good >= 240) {
            // Video/overlay proof captured; keep advancing until the game's boot reaches audio.
            uint64_t af = 0; int ans = 0;
            rp_runtime_audio_stats(rt, &af, &ans);
            if (ans || good >= 1500) break;
        }
    }
    fprintf(stderr, "[dolphin-core] presented %d good frames total\n", good); fflush(stderr);

    // Save the composited frame for human confirmation (the game inside our surface, overlay on top).
    if (!late.empty()) {
        FILE* fp = fopen("dolphin_core_composited.rgba", "wb");
        if (fp) { fwrite(late.data(), 1, late.size(), fp); fclose(fp); }
    }

    // Slice L: Dolphin's audio must reach the host through rp_host.audio_sample -> XAudio2 (device-
    // independent counters; they tally even with no output device). Read before teardown.
    uint64_t audio_frames = 0; int audio_nonsilent = 0;
    rp_runtime_audio_stats(rt, &audio_frames, &audio_nonsilent);
    fprintf(stderr, "[dolphin-core] audio: frames=%llu nonsilent=%d\n",
            (unsigned long long)audio_frames, audio_nonsilent); fflush(stderr);

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    // Proof: the game reached our composited surface through the Runtime/core path, rendered and advancing.
    REQUIRE(!late.empty());
    CHECK(looks_rendered(late));                    // real rendered content (not black/uniform)
    if (!early.empty()) {
        size_t diff = 0;
        for (size_t i = 0; i < late.size(); ++i) if (late[i] != early[i]) ++diff;
        CHECK(diff > late.size() / 20);             // frames differ -> real emulation advancing
    }
    // Overlay blend: the compositor draws a blue (0,0,1) @0.5-alpha quad over the top-left quadrant with
    // src_alpha/one_minus_src_alpha, so there out = 0.5*content + 0.5*(0,0,255): green is halved and blue
    // gets +127. Absolute blue barely moves on a near-white boot screen (already saturated), so assert
    // the brightness-robust signal the tint actually produces — blue-minus-green is far higher in the
    // overlaid top-left than in the untinted bottom-right (real alpha blending, not opaque layering).
    auto tint = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        double sb = 0, sg = 0; uint32_t n = 0;
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                sg += late[(y * W + x) * 4 + 1];
                sb += late[(y * W + x) * 4 + 2];
                ++n;
            }
        return n ? (sb - sg) / n : 0.0;  // blue-minus-green
    };
    double tint_tl = tint(0, 0, W / 2, H / 2), tint_br = tint(W / 2, H / 2, W, H);
    fprintf(stderr, "[dolphin-core] overlay tint (B-G): top-left=%.1f bottom-right=%.1f\n", tint_tl, tint_br);
    fflush(stderr);
    CHECK(tint_tl > tint_br + 30.0);

    // Dolphin's game audio reached RetroPark's output path, and it is real sound (not silence).
    CHECK(audio_frames > 0);
    CHECK(audio_nonsilent == 1);
}
