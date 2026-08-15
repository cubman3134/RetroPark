#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/gl/GLContext.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// B1 HW-render proof: Mupen64Plus-Next (GLideN64, desktop GL 3.3) renders a real N64 ROM into the shim's
// GL FBO; the shim reads it back to CPU RGBA and forwards it through the driven video_refresh path. Runs on
// the D3D11 host so the shim's GL context is independent of any host GL context. Gated RP_RUN_N64=1.
#ifndef RP_N64_CORE_DIR
#define RP_N64_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/build/cores/libretro_shim_n64"
#endif
#ifndef RP_N64_ROM
#define RP_N64_ROM "C:/Users/cubma/AppData/Local/Temp/n64rom/Banjo-Tooie (USA).n64"
#endif

namespace {
bool file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
uint64_t bytesum(const std::vector<uint8_t>& v) { uint64_t s = 0; for (uint8_t b : v) s += b; return s; }
}

TEST_CASE("hwrender e2e: Mupen64Plus-Next renders a real N64 ROM through GL readback (gated)") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping N64 HW-render e2e"); return; }
    if (!rp::GLContext::probe()) { WARN("no OpenGL 3.3; skipping"); return; }
    const char* coreDir = std::getenv("RP_N64_CORE_DIR") ? std::getenv("RP_N64_CORE_DIR") : RP_N64_CORE_DIR;
    const char* rom = std::getenv("RP_N64_ROM") ? std::getenv("RP_N64_ROM") : RP_N64_ROM;
    if (!file_exists((std::string(coreDir) + "/mupen64plus_next_libretro.dll").c_str())) { WARN("mupen core absent; skipping"); return; }
    if (!file_exists(rom)) { WARN("N64 ROM absent; skipping"); return; }

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);   // D3D11 host: shim's GL ctx stays independent
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    fprintf(stderr, "[n64] load_core(%s)\n", coreDir); fflush(stderr);
    REQUIRE(rp_runtime_load_core(rt, coreDir) == RP_OK);
    fprintf(stderr, "[n64] load_content(%s)\n", rom); fflush(stderr);
    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);

    // Feed a held abstract-pad input (Start + full-left analog) before pumping. Device-independent
    // proof the shim polled host input (mirrors the Dolphin input gate): assert poll_count>0 below.
    rp_input_state held{};
    held.pad_buttons = (1u << RP_PAD_START);
    held.pad_axes[RP_AXIS_LEFT_X] = -32767;
    rp_runtime_set_input(rt, 0, &held);

    std::vector<uint8_t> img((size_t)W * H * 4, 0), early, late;
    int good = 0;
    // N64 boot (PIF/IPL + game logos) takes many frames before real rendering; pump generously.
    for (int i = 0; i < 3000; ++i) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        ++good;
        if (good == 60) early = img;
        uint64_t s = bytesum(img);
        if (s > (uint64_t)W * H) late = img;                    // hold the most recent non-black frame
        if (good % 120 == 0) { fprintf(stderr, "[n64] %d frames, bytesum=%llu\n", good, (unsigned long long)s); fflush(stderr); }
        if (good >= 1200 && !late.empty() && !early.empty() && late != early) break;
    }
    fprintf(stderr, "[n64] presented %d good frames; late nonblank=%d\n", good, (int)!late.empty()); fflush(stderr);

    if (!late.empty()) { FILE* fp = fopen("n64_frame.rgba", "wb"); if (fp) { fwrite(late.data(),1,late.size(),fp); fclose(fp); } }

    uint64_t polls = rp_runtime_input_poll_count(rt);   // capture before teardown
    fprintf(stderr, "[n64] input_poll_count=%llu\n", (unsigned long long)polls); fflush(stderr);

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    REQUIRE(!late.empty());
    CHECK(bytesum(late) > (uint64_t)W * H);   // non-black
    REQUIRE(!early.empty());
    CHECK(late != early);                      // advancing
    CHECK(polls > 0);                          // the shim polled host input at least once
}
