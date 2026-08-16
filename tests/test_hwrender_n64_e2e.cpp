#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/gl/GLContext.h"
#include "render/gl/GLBackend.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// HW-render proof: Mupen64Plus-Next (GLideN64, desktop GL 3.3) renders a real N64 ROM into the shim's
// GL FBO. Two host paths, one shim:
//   * D3D11 host  (B1 fallback) -- the host has no GL context to share, so the shim reads its FBO back to
//     CPU RGBA and forwards it through the driven video_refresh path. gl_frame_count stays 0.
//   * OpenGL host (B2 zero-copy) -- the host shares its GL context, the shim hands back its FBO texture via
//     video_refresh_gl, and GLBackend composites it directly with no readback. gl_frame_count > 0.
// Gated RP_RUN_N64=1.
#ifndef RP_N64_CORE_DIR
#define RP_N64_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/build/cores/libretro_shim_n64"
#endif
#ifndef RP_N64_ROM
#define RP_N64_ROM "C:/Users/cubma/AppData/Local/Temp/n64rom/Banjo-Tooie (USA).n64"
#endif

namespace {
bool file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
uint64_t bytesum(const std::vector<uint8_t>& v) { uint64_t s = 0; for (uint8_t b : v) s += b; return s; }

// Shared setup: create the runtime on `api`, load the N64 shim + Banjo-Tooie, feed a held pad, pump until
// the frame advances, then report the last non-black frame plus stats. Returns false if the fixtures are
// absent (caller treats that as a skip). On success `late`/`early`/`polls`/`glFrames` are filled.
struct N64Result {
    std::vector<uint8_t> early, late;
    uint64_t polls = 0;
    uint64_t glFrames = 0;
};
bool run_n64(rp_graphics_api api, uint32_t W, uint32_t H, N64Result& out, const char* savePath) {
    const char* coreDir = std::getenv("RP_N64_CORE_DIR") ? std::getenv("RP_N64_CORE_DIR") : RP_N64_CORE_DIR;
    const char* rom = std::getenv("RP_N64_ROM") ? std::getenv("RP_N64_ROM") : RP_N64_ROM;
    if (!file_exists((std::string(coreDir) + "/mupen64plus_next_libretro.dll").c_str())) { WARN("mupen core absent; skipping"); return false; }
    if (!file_exists(rom)) { WARN("N64 ROM absent; skipping"); return false; }

    rp_runtime* rt = rp_runtime_create(api, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    fprintf(stderr, "[n64] api=%d load_core(%s)\n", (int)api, coreDir); fflush(stderr);
    REQUIRE(rp_runtime_load_core(rt, coreDir) == RP_OK);
    fprintf(stderr, "[n64] load_content(%s)\n", rom); fflush(stderr);
    REQUIRE(rp_runtime_load_content(rt, rom) == RP_OK);

    // Feed a held abstract-pad input (Start + full-left analog) before pumping. Device-independent
    // proof the shim polled host input (mirrors the Dolphin input gate): assert poll_count>0 below.
    rp_input_state held{};
    held.pad_buttons = (1u << RP_PAD_START);
    held.pad_axes[RP_AXIS_LEFT_X] = -32767;
    rp_runtime_set_input(rt, 0, &held);

    std::vector<uint8_t> img((size_t)W * H * 4, 0);
    int good = 0;
    // N64 boot (PIF/IPL + game logos) takes many frames before real rendering; pump generously.
    for (int i = 0; i < 3000; ++i) {
        if (rp_runtime_present(rt, img.data()) != RP_OK) continue;
        ++good;
        if (good == 60) out.early = img;
        uint64_t s = bytesum(img);
        if (s > (uint64_t)W * H) out.late = img;                // hold the most recent non-black frame
        if (good % 120 == 0) { fprintf(stderr, "[n64] %d frames, bytesum=%llu\n", good, (unsigned long long)s); fflush(stderr); }
        if (good >= 1200 && !out.late.empty() && !out.early.empty() && out.late != out.early) break;
    }
    out.glFrames = rp_runtime_gl_frame_count(rt);
    out.polls = rp_runtime_input_poll_count(rt);
    fprintf(stderr, "[n64] api=%d presented %d good frames; late nonblank=%d gl_frame_count=%llu input_poll_count=%llu\n",
            (int)api, good, (int)!out.late.empty(),
            (unsigned long long)out.glFrames, (unsigned long long)out.polls); fflush(stderr);

    if (savePath && !out.late.empty()) { FILE* fp = fopen(savePath, "wb"); if (fp) { fwrite(out.late.data(),1,out.late.size(),fp); fclose(fp); } }

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);
    return true;
}
}

// B1 fallback: D3D11 host, the shim's GL ctx is independent of any host GL context, so the shim reads its
// FBO back to CPU RGBA. Renders a real N64 ROM -- and gl_frame_count MUST be 0 (no zero-copy handoff).
TEST_CASE("hwrender e2e: Mupen64Plus-Next renders a real N64 ROM through GL readback on the D3D11 host (gated)") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping N64 HW-render e2e"); return; }
    if (!rp::GLContext::probe()) { WARN("no OpenGL 3.3; skipping"); return; }

    const uint32_t W = 640, H = 480;
    N64Result r;
    if (!run_n64(RP_GFX_D3D11, W, H, r, "n64_frame.rgba")) return;

    REQUIRE(!r.late.empty());
    CHECK(bytesum(r.late) > (uint64_t)W * H);   // non-black
    REQUIRE(!r.early.empty());
    CHECK(r.late != r.early);                    // advancing
    CHECK(r.polls > 0);                          // the shim polled host input at least once
    CHECK(r.glFrames == 0);                      // B1 fallback: no GL frame handoff on the D3D11 host
}

// B2 zero-copy: OpenGL host shares its GL context with the shim; Mupen hands its FBO texture back via
// video_refresh_gl and GLBackend composites it with no CPU readback. Renders a real N64 ROM AND
// gl_frame_count MUST be > 0 (zero-copy engaged). THIS IS THE LOAD-BEARING B2 PROOF.
TEST_CASE("hwrender e2e: Mupen64Plus-Next renders a real N64 ROM zero-copy on the OpenGL host (gated)") {
    if (!std::getenv("RP_RUN_N64")) { WARN("RP_RUN_N64 not set; skipping N64 zero-copy e2e"); return; }
    if (!rp::GLBackend::probe_gl_shared()) { WARN("no capable OpenGL 3.3 shared context; skipping"); return; }

    const uint32_t W = 640, H = 480;
    N64Result r;
    if (!run_n64(RP_GFX_OPENGL, W, H, r, "n64_zerocopy.rgba")) return;

    REQUIRE(!r.late.empty());
    CHECK(bytesum(r.late) > (uint64_t)W * H);   // non-black: real Banjo-Tooie rendered
    REQUIRE(!r.early.empty());
    CHECK(r.late != r.early);                    // advancing
    CHECK(r.polls > 0);                          // the shim polled host input at least once
    CHECK(r.glFrames > 0);                       // zero-copy engaged: shim handed GL textures, not readback
}
