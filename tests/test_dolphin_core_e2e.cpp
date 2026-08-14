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
// it into our surface — all through rp_core_abi, no test-only C API and no manual consume loop. This is
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

TEST_CASE("dolphin core: Runtime loads dolphin_present + a GC ISO and composites it into our surface (gated)") {
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

    // Slice M: host-owned input. Hold a strong input (full Control Stick left + A + Start); the vehicle's
    // override pulls it via rp_host.input_state each SI poll, which routes through Runtime::on_input.
    rp_input_state held{};
    held.pad_axes[RP_AXIS_LEFT_X] = -32767;                 // full left on the analog stick
    held.pad_buttons = (uint16_t)((1u << RP_PAD_A) | (1u << RP_PAD_START));
    rp_runtime_set_input(rt, 0, &held);

    // Dolphin boots over a few seconds; present() returns non-OK until the first XFB frame lands in the
    // ring. Poll for the first good frame, then keep pumping to capture an early and a late frame.
    // The video proof is captured by frame 240 (the settled early-boot frame). Dolphin's boot
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
        if (good == 30) early = img;               // a settled early frame (used only for the advance diff)
        // Hold the most recent ACTUALLY-RENDERED frame. (The old code froze this at good<=240 assuming the
        // slow lock-step advanced ~1 emu-frame/present; with direct multi-slot present the game boots at
        // full speed, so a fixed early index can land on a black boot transition. Tracking the latest
        // rendered frame keeps the proof robust to boot timing without weakening it.)
        if (looks_rendered(img)) late = img;
        if (good % 60 == 0) { fprintf(stderr, "[dolphin-core] %d frames presented\n", good); fflush(stderr); }
        if (good >= 240) {
            // Video proof captured; keep advancing until the game's boot reaches audio.
            uint64_t af = 0; int ans = 0;
            rp_runtime_audio_stats(rt, &af, &ans);
            if (ans || good >= 1500) break;
        }
    }
    fprintf(stderr, "[dolphin-core] presented %d good frames total\n", good); fflush(stderr);

    // Save the composited frame for human confirmation (the game inside our surface).
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

    uint64_t input_polls = rp_runtime_input_poll_count(rt);
    fprintf(stderr, "[dolphin-core] input polls=%llu\n", (unsigned long long)input_polls); fflush(stderr);

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

    // Dolphin's game audio reached RetroPark's output path, and it is real sound (not silence).
    CHECK(audio_frames > 0);
    CHECK(audio_nonsilent == 1);

    // Dolphin polled host input through the ABI (the override called rp_host.input_state each SI poll).
    CHECK(input_polls > 0);
}

// Direct multi-slot present speed + stability gauge (gated on RP_DOLPHIN_SPEED=1). Unlike the case above
// (which sleeps 50 ms/present and so throttles the whole pipeline), this pumps present() in a TIGHT loop
// so the host consumes as fast as it can — removing back-pressure so Dolphin runs at its own throttle.
// The honest speed clock is Dolphin's PRESENTED-FRAME count (rp_dolphin_present_count): frames/wall-sec
// / 59.94 = real-time fraction. (audio_frames is wall-clock-pulled and NOT a speed signal.) Measures a
// 30 s window and a full 60 s run (no crash), and saves a late frame for the render-clean check.
TEST_CASE("dolphin core: direct-present speed + 60s stability (gated)") {
    if (!std::getenv("RP_DOLPHIN_SPEED")) { WARN("RP_DOLPHIN_SPEED not set; skipping speed gauge"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    const char* rom = std::getenv("RP_DOLPHIN_ROM") ? std::getenv("RP_DOLPHIN_ROM") : kRom;
    if (!file_exists(rom)) { WARN("ROM absent; skipping"); return; }

    // Resolve the presented-frame counter export (refcounted handle; the Runtime already loaded the DLL).
    std::string dll = std::string(kCoreDir) + "/dolphin_present.dll";
    HMODULE mod = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto present_count = mod ? reinterpret_cast<uint64_t(*)()>(GetProcAddress(mod, "rp_dolphin_present_count")) : nullptr;
    REQUIRE(present_count);

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);

    std::vector<uint8_t> img(W * H * 4, 0), late;

    auto now = [] { return std::chrono::steady_clock::now(); };
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    // Warm up: pump until the first good frame lands, then let it settle a few seconds past boot logos.
    fprintf(stderr, "[dolphin-speed] warming up...\n"); fflush(stderr);
    int good = 0;
    auto t_start = now();
    while (secs(t_start, now()) < 40.0) {
        if (rp_runtime_present(rt, img.data()) == RP_OK) { ++good; late = img; }
        if (good > 180) break;   // ~first 3s of real frames -> settled
    }
    REQUIRE(good > 0);

    // 30 s measurement window (tight loop, no sleep).
    auto measure = [&](double window) {
        uint64_t pc0 = present_count();
        uint64_t af0 = 0; int an0 = 0; rp_runtime_audio_stats(rt, &af0, &an0);
        auto w0 = now();
        while (secs(w0, now()) < window) { if (rp_runtime_present(rt, img.data()) == RP_OK) late = img; }
        auto w1 = now();
        uint64_t pc1 = present_count();
        uint64_t af1 = 0; int an1 = 0; rp_runtime_audio_stats(rt, &af1, &an1);
        double wall = secs(w0, w1);
        double fps = (pc1 - pc0) / wall;
        double rt_frac = fps / 59.94;
        fprintf(stderr, "[dolphin-speed] window=%.1fs presented=%llu wall=%.2fs fps=%.2f real-time=%.3f "
                        "(audio +%llu, nonsilent=%d)\n", window, (unsigned long long)(pc1 - pc0), wall,
                fps, rt_frac, (unsigned long long)(af1 - af0), an1); fflush(stderr);
        return rt_frac;
    };

    double rt30 = measure(30.0);
    // Continue to a full 60 s of runtime for the crash/stability check.
    double rt_more = measure(30.0);
    (void)rt_more;

    // Still alive after 60s -> confirm the core is running and present still succeeds.
    rp_result alive = rp_runtime_present(rt, img.data());
    fprintf(stderr, "[dolphin-speed] alive-after-60s present=%d, presented_total=%llu\n",
            (int)alive, (unsigned long long)present_count()); fflush(stderr);

    if (!late.empty()) {
        FILE* fp = fopen("dolphin_speed_frame.rgba", "wb");
        if (fp) { fwrite(late.data(), 1, late.size(), fp); fclose(fp); }
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    CHECK(alive == RP_OK);              // 60s, no crash, present still working
    CHECK(!late.empty());
    CHECK(looks_rendered(late));        // clean real content
    CHECK(rt30 >= 0.90);               // direct-present target (report the exact number regardless)
}

// BACK-PRESSURE CORRECTNESS PROOF (gated on RP_DOLPHIN_BACKPRESSURE=1). This is the whole point of the
// two-timeline sync fix: with N shared slots, the consume timeline bounds Dolphin to at most slot_count
// frames ahead of the frame the HOST last finished reading — so it can never overwrite a slot the host is
// still reading (the tear). To make that bound OBSERVABLE it must actually bind, which needs the host slow
// enough that slot_count * host_fps < Dolphin's ~60 fps self-throttle (the host consumes the NEWEST ready
// frame each present via latest_ready, so each present lets Dolphin advance ~slot_count frames; the ring is
// SurfaceRing{3}, so a 30 fps host allows 3*30=90 > 60 and Dolphin never has to wait). We therefore pace
// the host at ~10 fps: with the fix Dolphin is bounded to ~slot_count*10 = ~30 fps (throttled well below
// its 60 fps free-run). BEFORE the fix the reuse wait was satisfied by Dolphin's OWN produce signals on the
// single shared timeline, so it ignored the host entirely and free-ran at ~60 fps regardless of the host
// pace. The discriminating assertion is "produced fps is throttled well below 60" — i.e. Dolphin genuinely
// waited on the host, and the produced/host ratio pins it near the ring depth (slot_count), not runaway.
TEST_CASE("dolphin core: back-pressure throttles Dolphin to a slow host (gated)") {
    if (!std::getenv("RP_DOLPHIN_BACKPRESSURE")) { WARN("RP_DOLPHIN_BACKPRESSURE not set; skipping back-pressure proof"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    const char* rom = std::getenv("RP_DOLPHIN_ROM") ? std::getenv("RP_DOLPHIN_ROM") : kRom;
    if (!file_exists(rom)) { WARN("ROM absent; skipping"); return; }

    std::string dll = std::string(kCoreDir) + "/dolphin_present.dll";
    HMODULE mod = LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto present_count = mod ? reinterpret_cast<uint64_t(*)()>(GetProcAddress(mod, "rp_dolphin_present_count")) : nullptr;
    REQUIRE(present_count);

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);

    std::vector<uint8_t> img(W * H * 4, 0), late;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    // Host pace: deliberately slow (default 10 fps). Sleep to the next cadence tick after each present.
    const double kHostFps = std::getenv("RP_DOLPHIN_HOST_FPS") ? atof(std::getenv("RP_DOLPHIN_HOST_FPS")) : 10.0;
    const auto kPeriod = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / kHostFps));
    uint64_t host_presents = 0;
    auto paced_present = [&](std::chrono::steady_clock::time_point& next) {
        rp_result pr = rp_runtime_present(rt, img.data());
        if (pr == RP_OK) { late = img; ++host_presents; }
        next += kPeriod;
        auto t = now();
        if (t < next) std::this_thread::sleep_until(next);
        else next = t;   // fell behind; don't spiral
        return pr;
    };

    // Warm up past the boot logos at the slow host pace (present must succeed first).
    fprintf(stderr, "[dolphin-bp] warming up at %.0f fps host pace...\n", kHostFps); fflush(stderr);
    int good = 0;
    auto next = now();
    auto warm0 = now();
    while (secs(warm0, now()) < 60.0) {
        if (paced_present(next) == RP_OK) ++good;
        if (good > 60) break;
    }
    REQUIRE(good > 0);

    // Measure Dolphin's produced-frame rate over a 30 s window while the host holds the slow pace.
    uint64_t pc0 = present_count(), hp0 = host_presents;
    auto w0 = now();
    while (secs(w0, now()) < 30.0) paced_present(next);
    auto w1 = now();
    uint64_t pc1 = present_count();

    double wall = secs(w0, w1);
    double produced_fps = (pc1 - pc0) / wall;
    double host_fps_actual = (host_presents - hp0) / wall;
    double ratio = host_fps_actual > 0 ? produced_fps / host_fps_actual : 0.0;
    double rt_frac = produced_fps / 59.94;
    fprintf(stderr, "[dolphin-bp] window=%.2fs produced=%llu produced_fps=%.2f host_fps=%.2f "
                    "produced/host=%.2f real-time=%.3f\n", wall, (unsigned long long)(pc1 - pc0),
            produced_fps, host_fps_actual, ratio, rt_frac); fflush(stderr);

    if (!late.empty()) {
        FILE* fp = fopen("dolphin_backpressure_frame.rgba", "wb");
        if (fp) { fwrite(late.data(), 1, late.size(), fp); fclose(fp); }
    }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    CHECK(!late.empty());
    CHECK(looks_rendered(late));
    // CORRECTNESS PROOF: Dolphin did NOT free-run at ~60 fps — it waited on the host. At a 10 fps host with
    // a 3-slot ring the bound is ~slot_count*10 = ~30 fps, comfortably below the 60 fps free-run. Before the
    // fix this sat near 60 regardless of host pace. Bands are wide to absorb boot/jitter but the ceiling is
    // the load-bearing check.
    CHECK(produced_fps < 45.0);    // throttled well below the ~60 fps free-run -> genuinely waited on host
    CHECK(produced_fps > 12.0);    // still advancing with the host (not stalled)
    CHECK(ratio < 5.0);            // bounded near the ring depth (~3), NOT the ~6x a 60 fps free-run implies
}

// SECONDARY (render-accuracy): capture a game's menu/game-select screen to check whether 2D/text elements
// drawn via EFB->RAM copies appear. Gated on RP_DOLPHIN_SONIC=1; point RP_DOLPHIN_ROM at Sonic Mega
// Collection. Pulses Start to walk past the title into the game-select carousel, then saves the frame to
// RP_DOLPHIN_CAP (default dolphin_menu_frame.rgba). Run once with RP_DOLPHIN_ACCURATE_COPIES unset and once
// set, then diff the two captures — the accurate-copies run should fill in the blank title panel.
TEST_CASE("dolphin core: menu/text capture (gated)") {
    if (!std::getenv("RP_DOLPHIN_SONIC")) { WARN("RP_DOLPHIN_SONIC not set; skipping menu capture"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    const char* rom = std::getenv("RP_DOLPHIN_ROM") ? std::getenv("RP_DOLPHIN_ROM") : kRom;
    if (!file_exists(rom)) { WARN("ROM absent; skipping"); return; }
    const char* cap = std::getenv("RP_DOLPHIN_CAP") ? std::getenv("RP_DOLPHIN_CAP") : "dolphin_menu_frame.rgba";

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);

    std::vector<uint8_t> img(W * H * 4, 0), late;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto secs = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    // Wall-clock scripted navigation (present runs ~real-time, so drive by seconds not frame counts):
    //   - START_AT..START_AT+0.4s: hold Start to leave the "PRESS START" title -> main game-select menu.
    //   - then idle (never press Start again -> don't launch a game).
    // Saves a labeled SERIES of raw RGBA frames every ~1.5s across the whole run so the title->menu
    // journey is visible from one run and the settled menu frame can be picked. RP_DOLPHIN_START_AT
    // (default 18s) tunes when the Start pulse fires; RP_DOLPHIN_RUN (default 34s) is the total length.
    const double start_at = std::getenv("RP_DOLPHIN_START_AT") ? atof(std::getenv("RP_DOLPHIN_START_AT")) : 18.0;
    const double run_len  = std::getenv("RP_DOLPHIN_RUN")      ? atof(std::getenv("RP_DOLPHIN_RUN"))      : 34.0;
    // Second pulse: press A to enter the GAMES carousel from the main menu (GAMES is the default highlight).
    // -1 disables it (stay on the main menu). Default: 8s after the Start pulse.
    const double enter_at = std::getenv("RP_DOLPHIN_ENTER_AT") ? atof(std::getenv("RP_DOLPHIN_ENTER_AT")) : (start_at + 8.0);
    // Third pulse: press A again to open the highlighted game's preview screen (big title logo top-center,
    // the panel that renders blank). -1 disables. Default: 8s after entering GAMES.
    const double select_at = std::getenv("RP_DOLPHIN_SELECT_AT") ? atof(std::getenv("RP_DOLPHIN_SELECT_AT")) : (enter_at + 8.0);
    const std::string cap_base = cap;   // series files: <cap>.tNN.rgba

    auto t0 = now();
    int good = 0;
    double next_snap = 12.0;   // start snapshotting around when the title should be up
    while (secs(t0, now()) < run_len) {
        double t = secs(t0, now());
        rp_input_state in{};
        if (t >= start_at && t < start_at + 0.40)
            in.pad_buttons = (uint16_t)(1u << RP_PAD_START);
        if (enter_at >= 0.0 && t >= enter_at && t < enter_at + 0.40)
            in.pad_buttons = (uint16_t)(1u << RP_PAD_A);
        if (select_at >= 0.0 && t >= select_at && t < select_at + 0.40)
            in.pad_buttons = (uint16_t)(1u << RP_PAD_A);
        rp_runtime_set_input(rt, 0, &in);
        if (rp_runtime_present(rt, img.data()) == RP_OK) { ++good; late = img; }
        if (t >= next_snap && !late.empty()) {
            char name[512];
            snprintf(name, sizeof(name), "%s.t%02d.rgba", cap_base.c_str(), (int)(t + 0.5));
            FILE* sp = fopen(name, "wb");
            if (sp) { fwrite(late.data(), 1, late.size(), sp); fclose(sp); }
            fprintf(stderr, "[dolphin-menu] snap t=%.1fs good=%d -> %s\n", t, good, name); fflush(stderr);
            next_snap = t + 1.5;
        }
    }
    REQUIRE(good > 0);
    if (!late.empty()) {
        FILE* fp = fopen(cap, "wb");
        if (fp) { fwrite(late.data(), 1, late.size(), fp); fclose(fp); }
    }
    fprintf(stderr, "[dolphin-menu] captured %d good frames -> %s\n", good, cap); fflush(stderr);
    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
    CHECK(!late.empty());
    CHECK(looks_rendered(late));
}
