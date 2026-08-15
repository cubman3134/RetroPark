# RetroPark HW-Render libretro Cores (B1: readback) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The libretro shim implements GL hardware-render: accept `SET_HW_RENDER` for desktop GL, hand the core
an FBO (color + depth) via `get_current_framebuffer`/`get_proc_address`, drive `context_reset`, then each frame
render the core into the FBO, `glReadPixels` + row-flip to CPU RGBA, and forward through the unchanged driven
`video_refresh` path. Proven with **Mupen64Plus-Next (N64)** on the D3D11 host. NES/SW unregressed. RetroPark only.

**Architecture:** A new isolated `cores/libretro_shim/HwRenderGL.{h,cpp}` owns the GL side (reusing A's
`src/render/gl/GLContext`, compiled into the shim). `LibretroShim.cpp` gains the HW-render hooks. `GLFns` gains
4 renderbuffer entrypoints (additive). Readback → the existing driven path, so any host backend composites it.

**Tech Stack:** C++17, A's hand-rolled WGL `GLContext` (no GL loader dep), system `opengl32`/`gdi32`, libretro
HW-render env API, doctest, CMake/MSBuild.

**Reference spec:** `docs/superpowers/specs/2026-08-15-retropark-hwrender-libretro-b1-design.md`.

## Global Constraints

- **Desktop GL only.** Accept `RETRO_HW_CONTEXT_OPENGL` (1) and `RETRO_HW_CONTEXT_OPENGL_CORE` (3); reject
  everything else (GLES/Vulkan/none) from `SET_HW_RENDER` (`return false`). GLES/ANGLE is B-later.
- **No new third-party dependency.** GL entrypoints via A's `GLFns` (loaded through `wglGetProcAddress`); the
  core loads its OWN GL via the `get_proc_address` the shim provides. Link only system `opengl32`/`gdi32`.
- **No ABI change** — B1 is entirely inside the shim + host GL helper; `RETRO_PARK_ABI_VERSION` stays 7. The
  runtime sees an ordinary driven core emitting CPU RGBA.
- **SW path byte-unchanged.** A core that never calls `SET_HW_RENDER` must behave exactly as today.
- **GL is bottom-left origin.** Honor `hw_render.bottom_left_origin`: flip readback rows when true (the GL
  default), don't when false. `out_rgba` to the runtime is top-origin (like the SW path).
- **Cores/ROMs are never committed** (git-ignored `external/libretro-cores/`, `C:/RetroBat/roms`).
- No AI attribution in commits. At slice end merge to `main` + push (RetroPark convention).

---

### Task 1: `GLFns` additive extensions — renderbuffer entrypoints + a GL-version parameter

**Files:**
- Modify: `src/render/gl/GLContext.h` (4 renderbuffer PFNs in `GLFns`; `initialize` gains `major`/`minor`)
- Modify: `src/render/gl/GLContext.cpp` (load the 4 PFNs; pass major/minor into the context attribs)
- Test: extend `tests/test_gl_context.cpp`

**Interfaces:**
- Produces: `GLFns` gains `GenRenderbuffers`/`BindRenderbuffer`/`RenderbufferStorage`/`FramebufferRenderbuffer`;
  `GLContext::initialize(void*, uint32_t, uint32_t, std::string&, int major=3, int minor=3)`.
- Consumes: nothing new.

- [ ] **Step 1: Extend `GLFns` in `GLContext.h`** — after `ActiveTexture`, add:

```cpp
    void (WINAPI *GenRenderbuffers)(GLsizei,GLuint*);
    void (WINAPI *BindRenderbuffer)(GLenum,GLuint);
    void (WINAPI *RenderbufferStorage)(GLenum,GLenum,GLsizei,GLsizei);
    void (WINAPI *FramebufferRenderbuffer)(GLenum,GLenum,GLenum,GLuint);
    void (WINAPI *Enable)(GLenum);
    void (WINAPI *Disable)(GLenum);
    void (WINAPI *Scissor)(GLint,GLint,GLsizei,GLsizei);
```

(`Enable`/`Disable`/`Scissor` are GL 1.0 — used by Task 2's flip test's `test_fill`.)

  And change the `initialize` declaration to:
  `bool initialize(void* native_window, uint32_t w, uint32_t h, std::string& err, int major = 3, int minor = 3);`

- [ ] **Step 2: Load them + honor the version in `GLContext.cpp`** — in the `LD(...)` block, add:

```cpp
    LD(GenRenderbuffers,"glGenRenderbuffers") LD(BindRenderbuffer,"glBindRenderbuffer")
    LD(RenderbufferStorage,"glRenderbufferStorage") LD(FramebufferRenderbuffer,"glFramebufferRenderbuffer")
    LD(Enable,"glEnable") LD(Disable,"glDisable") LD(Scissor,"glScissor")
```

  In `initialize`, replace the hardcoded `3,3` in `ctxAttribs` with the params (clamp to a 3.3-core floor so an
  older request can't ask for a profile GLFns can't serve):

```cpp
    int cmaj = major, cmin = minor;
    if (cmaj < 3 || (cmaj == 3 && cmin < 3)) { cmaj = 3; cmin = 3; }
    const int ctxAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, cmaj, WGL_CONTEXT_MINOR_VERSION_ARB, cmin,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0 };
```

  Update the `initialize` signature in the .cpp to match the header (`, int major, int minor`). `probe()` and
  the A backend keep calling with the defaults — unaffected.

- [ ] **Step 3: Extend the context test** (`tests/test_gl_context.cpp`) — after the existing 3.3 assertions, add
  that the renderbuffer entrypoints loaded:

```cpp
    CHECK(ctx.gl().GenRenderbuffers != nullptr);
    CHECK(ctx.gl().RenderbufferStorage != nullptr);
    CHECK(ctx.gl().FramebufferRenderbuffer != nullptr);
    CHECK(ctx.gl().BindRenderbuffer != nullptr);
```

- [ ] **Step 4: Build + run** → `cmake --build build --config Release --target retropark_tests` then
  `./build/tests/Release/retropark_tests.exe -tc="gl context*"` → PASS (or skip). Full suite still green
  (the A backend + compositor still build/link with the extended GLFns).
- [ ] **Step 5: Commit** (`feat(gl): GLFns renderbuffer entrypoints + selectable context version`).

---

### Task 2: `HwRenderGL` — GL context + FBO (color+depth) + readback/flip

**Files:**
- Create: `cores/libretro_shim/HwRenderGL.h`, `cores/libretro_shim/HwRenderGL.cpp`
- Test: `tests/test_hwrender_gl.cpp` (add to `retropark_tests`)

**Interfaces:**
- Consumes: `rp::GLContext` / `GLFns`.
- Produces: `rp::HwRenderGL` — `setup(depth,stencil,bottom_left_origin,maxW,maxH,major,minor,err)`,
  `fbo_id()`, `make_current()`, `read_frame(w,h,out_pitch) -> const void*`.

- [ ] **Step 1: `HwRenderGL.h`:**

```cpp
#pragma once
#include "render/gl/GLContext.h"
#include <vector>
#include <string>
#include <cstdint>
namespace rp {
// Owns the GL context + FBO a HW-render libretro core draws into, and reads it back to CPU RGBA. Isolated
// from LibretroShim's core-loading logic. Headless (its own GLContext) so it works under any host backend.
class HwRenderGL {
public:
    bool setup(bool depth, bool stencil, bool bottom_left_origin,
               uint32_t maxW, uint32_t maxH, int major, int minor, std::string& err);
    unsigned fbo_id() const { return fbo_; }              // for get_current_framebuffer
    bool make_current() { return ctx_.make_current(); }   // before each retro_run
    // Read the w*h region of the FBO to top-origin RGBA8; returns an internal buffer valid until the next
    // call. out_pitch = w*4. Flips rows iff the core uses bottom-left origin (GL default).
    const void* read_frame(uint32_t w, uint32_t h, uint32_t& out_pitch);
    uint32_t max_w() const { return maxW_; }
    uint32_t max_h() const { return maxH_; }
private:
    GLContext ctx_;
    unsigned fbo_ = 0, color_ = 0, depth_rb_ = 0;
    uint32_t maxW_ = 0, maxH_ = 0;
    bool blo_ = true;
    std::vector<uint8_t> read_, flip_;
};
} // namespace rp
```

- [ ] **Step 2: Write the failing test** (`tests/test_hwrender_gl.cpp`) — set up the FBO, draw a two-tone
  image (top half red via scissor, bottom half green) directly into it, and assert `read_frame` returns it
  **top-origin** (red on top) — this proves both readback AND the bottom-left→top-left flip (a solid color
  couldn't). GL enums inline.

```cpp
#include <doctest/doctest.h>
#include "../cores/libretro_shim/HwRenderGL.h"
#include "render/gl/GLContext.h"
using namespace rp;
TEST_CASE("hwrender gl: FBO readback is top-origin (flip works)") {
    if (!GLContext::probe()) { WARN("no GL 3.3"); return; }
    HwRenderGL hw; std::string err;
    REQUIRE(hw.setup(/*depth*/true,/*stencil*/false,/*bottom_left_origin*/true, 8, 8, 3, 3, err));
    REQUIRE(hw.make_current());
    // Bind our FBO (id from fbo_id) and paint bottom half red, top half green using scissor+clear.
    // (In GL's bottom-origin FBO, y=0..3 is the BOTTOM; y=4..7 the TOP.)
    // We reach GL through a fresh GLContext's GLFns... simplest: expose a tiny paint helper OR use glClear
    // via the fns. Use the context's fns:
    // -- get fns through a second GLContext? No: reuse hw's context which is current. Add a test accessor.
    // For the test, HwRenderGL exposes gl()+fbo for painting (test-only), OR we paint via a friend helper.
    // Simplest robust approach: HwRenderGL::test_fill(topR,topG,topB, botR,botG,botB) that scissor-clears.
    hw.test_fill(0,255,0, 255,0,0);   // top green, bottom red (in visual/top-origin terms)
    uint32_t pitch = 0;
    const uint8_t* px = static_cast<const uint8_t*>(hw.read_frame(8, 8, pitch));
    REQUIRE(px); REQUIRE(pitch == 8*4);
    // Row 0 is the TOP of the image -> should be green; last row -> red.
    CHECK(px[1] > 200); CHECK(px[0] < 60);                       // row 0 green
    const uint8_t* last = px + (size_t)7*pitch;
    CHECK(last[0] > 200); CHECK(last[1] < 60);                   // row 7 red
}
```

  Note: add a small **test-only** `void test_fill(uint8_t tr,uint8_t tg,uint8_t tb, uint8_t br,uint8_t bg,uint8_t bb);`
  to `HwRenderGL` that binds the FBO and scissor-clears the top-origin top half to (tr,tg,tb) and the
  bottom half to (br,bg,bb) — i.e. it clears the GL-BOTTOM half (y=0..h/2) to the "bottom" color and the
  GL-TOP half to the "top" color, matching how a bottom-left-origin core would render, so `read_frame`'s flip
  must put the "top" color at output row 0. (This helper exists only to exercise the flip; it is not used by
  the shim.)

  Run → FAIL (no `HwRenderGL.h`). (Add `tests/test_hwrender_gl.cpp` to the test target's sources.)

- [ ] **Step 3: `HwRenderGL.cpp`** — GL enums inline; setup creates the context (honoring major/minor), the
  color texture, an optional depth (or packed depth-stencil) renderbuffer, attaches both, checks completeness.

```cpp
#include "../cores/libretro_shim/HwRenderGL.h"   // adjust include to "HwRenderGL.h" when compiled in the shim TU
namespace rp {
enum { GL_TEXTURE_2D=0x0DE1, GL_RGBA8=0x8058, GL_RGBA=0x1908, GL_UBYTE=0x1401, GL_NEAREST=0x2600,
       GL_MIN=0x2801, GL_MAG=0x2800, GL_FRAMEBUFFER=0x8D40, GL_COLOR_ATTACH0=0x8CE0, GL_DEPTH_ATTACH=0x8D00,
       GL_DEPTH_STENCIL_ATTACH=0x821A, GL_RENDERBUFFER=0x8D41, GL_DEPTH24=0x81A6, GL_DEPTH24_STENCIL8=0x88F0,
       GL_FB_COMPLETE=0x8CD5, GL_PACK_ALIGNMENT=0x0D05, GL_COLOR_BUFFER_BIT=0x4000, GL_SCISSOR_TEST=0x0C11 };

bool HwRenderGL::setup(bool depth, bool stencil, bool blo, uint32_t maxW, uint32_t maxH,
                       int major, int minor, std::string& err) {
    maxW_ = maxW ? maxW : 1; maxH_ = maxH ? maxH : 1; blo_ = blo;
    if (!ctx_.initialize(nullptr, maxW_, maxH_, err, major, minor)) return false;
    if (!ctx_.make_current()) { err = "make_current"; return false; }
    const GLFns& g = ctx_.gl();
    g.GenFramebuffers(1, &fbo_); g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    g.GenTextures(1, &color_); g.BindTexture(GL_TEXTURE_2D, color_);
    g.TexParameteri(GL_TEXTURE_2D, GL_MIN, GL_NEAREST); g.TexParameteri(GL_TEXTURE_2D, GL_MAG, GL_NEAREST);
    g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)maxW_, (GLsizei)maxH_, 0, GL_RGBA, GL_UBYTE, nullptr);
    g.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACH0, GL_TEXTURE_2D, color_, 0);
    if (depth) {
        g.GenRenderbuffers(1, &depth_rb_); g.BindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        // Only-stencil is invalid per libretro; a packed 24/8 covers depth or depth+stencil.
        GLenum fmt = stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH24;
        GLenum att = stencil ? GL_DEPTH_STENCIL_ATTACH : GL_DEPTH_ATTACH;
        g.RenderbufferStorage(GL_RENDERBUFFER, fmt, (GLsizei)maxW_, (GLsizei)maxH_);
        g.FramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, depth_rb_);
    }
    if (g.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FB_COMPLETE) { err = "incomplete FBO"; return false; }
    return true;
}

const void* HwRenderGL::read_frame(uint32_t w, uint32_t h, uint32_t& out_pitch) {
    if (w > maxW_) w = maxW_; if (h > maxH_) h = maxH_;
    out_pitch = w * 4;
    const GLFns& g = ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    g.PixelStorei(GL_PACK_ALIGNMENT, 1);
    read_.assign((size_t)w * h * 4, 0);
    g.ReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UBYTE, read_.data());
    if (!blo_) return read_.data();                 // core uses top-left origin: no flip
    flip_.assign((size_t)w * h * 4, 0);             // GL bottom-origin -> flip to top-origin
    const size_t row = (size_t)w * 4;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(flip_.data() + (size_t)y*row, read_.data() + (size_t)(h-1-y)*row, row);
    return flip_.data();
}

void HwRenderGL::test_fill(uint8_t tr,uint8_t tg,uint8_t tb, uint8_t br,uint8_t bg,uint8_t bb) {
    const GLFns& g = ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    // GL bottom half (y=0..h/2) = the visual BOTTOM; top half = visual TOP. Paint accordingly.
    // (Requires Enable/Disable + Scissor in GLFns; if absent, add them in Task 1 or clear whole then half.)
    g.Viewport(0,0,(GLsizei)maxW_,(GLsizei)maxH_);
    g.Enable(GL_SCISSOR_TEST);
    g.Scissor(0,0,(GLsizei)maxW_,(GLsizei)(maxH_/2));           // bottom half
    g.ClearColor(br/255.f,bg/255.f,bb/255.f,1.f); g.Clear(GL_COLOR_BUFFER_BIT);
    g.Scissor(0,(GLsizei)(maxH_/2),(GLsizei)maxW_,(GLsizei)(maxH_-maxH_/2)); // top half
    g.ClearColor(tr/255.f,tg/255.f,tb/255.f,1.f); g.Clear(GL_COLOR_BUFFER_BIT);
    g.Disable(GL_SCISSOR_TEST);
}
} // namespace rp
```

  **Note:** `test_fill` uses `Enable`/`Disable`/`Scissor` — if those aren't in `GLFns`, add them in Task 1
  alongside the renderbuffer PFNs (`void (WINAPI *Enable)(GLenum)`, `*Disable`, `*Scissor`,
  `*ClearColor` already exists). Add the `test_fill` decl to `HwRenderGL.h` behind a comment marking it
  test-only. (`Viewport`/`Clear`/`ClearColor` already exist in GLFns.)

- [ ] **Step 4: Build + run** the hwrender-gl test → PASS (top green / bottom red proves the flip). Full suite green.
- [ ] **Step 5: Commit** (`feat(shim): HwRenderGL — GL FBO for HW-render cores + top-origin readback`).

---

### Task 3: Shim HW-render hooks — accept GL, provide FBO/proc, context_reset, readback video path

**Files:**
- Modify: `cores/libretro_shim/LibretroShim.cpp`
- Modify: `cores/libretro_shim/CMakeLists.txt`

**Interfaces:**
- Consumes: `rp::HwRenderGL`, `retro_hw_render_callback`, `RETRO_HW_CONTEXT_OPENGL/_CORE`,
  `RETRO_HW_FRAME_BUFFER_VALID`.
- Produces: no ABI change; the shim now serves desktop-GL HW-render cores as ordinary driven cores.

- [ ] **Step 1: `Shim` struct fields** (in the `struct Shim { ... }` near line 21) — add:

```cpp
    // HW-render (B1). Set when a core requests desktop-GL SET_HW_RENDER; the shim renders the core into
    // HwRenderGL's FBO and reads it back to CPU RGBA, so the runtime still sees a driven core.
    retro_hw_render_callback hw_cb{};
    bool hw_requested = false;
    std::unique_ptr<HwRenderGL> hw;
```

  Add `#include "HwRenderGL.h"` and `#include <memory>` at the top.

- [ ] **Step 2: The frontend GL getters (global C fns, above `env_cb`):**

```cpp
// get_current_framebuffer: the FBO the core renders into (0 before setup / for SW cores).
static uintptr_t hw_get_current_framebuffer() { return g && g->hw ? g->hw->fbo_id() : 0; }
// get_proc_address: the core loads its OWN GL through this (wgl first, opengl32 fallback for GL 1.1 syms).
static retro_proc_address_t hw_get_proc_address(const char* sym) {
    if (!sym) return nullptr;
    void* p = (void*)wglGetProcAddress(sym);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        static HMODULE gl = GetModuleHandleA("opengl32.dll");
        p = (void*)GetProcAddress(gl, sym);
    }
    return reinterpret_cast<retro_proc_address_t>(p);
}
```

  (Add `#include <windows.h>` if not already pulled in via GLContext.h; GLContext.h defines WIN32_LEAN_AND_MEAN.)

- [ ] **Step 3: `env_cb` — replace the `SET_HW_RENDER` case** (currently `return false;`):

```cpp
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* cb = static_cast<retro_hw_render_callback*>(data);
            if (!cb) return false;
            // Desktop GL only (B1). GLES/Vulkan/etc. -> reject; the core falls back to SW or fails.
            if (cb->context_type != RETRO_HW_CONTEXT_OPENGL &&
                cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE)
                return false;
            g->hw_cb = *cb;
            g->hw_requested = true;
            g->hw_cb.get_current_framebuffer = hw_get_current_framebuffer;
            g->hw_cb.get_proc_address = hw_get_proc_address;
            // The core reads get_current_framebuffer/get_proc_address back from ITS struct, so write them
            // into the caller's struct too:
            cb->get_current_framebuffer = hw_get_current_framebuffer;
            cb->get_proc_address = hw_get_proc_address;
            return true;
        }
```

- [ ] **Step 4: `video_cb` — add the HW sentinel branch** (before the dupe check):

```cpp
void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {          // HW-render: the core drew into our FBO
        uint32_t p = 0;
        const void* rgba = g->hw ? g->hw->read_frame(w, h, p) : nullptr;
        if (rgba) g->host.video_refresh(g->host.host, rgba, w, h, p);
        else      g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        return;
    }
    if (!data || w == 0 || h == 0) {   // duplicate frame (SW or HW dupe)
        g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        return;
    }
    g->rgba.assign((size_t)w * h * 4, 0);
    rp::convert_to_rgba8(data, w, h, (uint32_t)pitch, g->pixel_format, g->rgba.data());
    g->host.video_refresh(g->host.host, g->rgba.data(), w, h, w * 4);
}
```

- [ ] **Step 5: `sh_load_content` — set up GL + `context_reset` after `retro_load_game`.** Find where
  `retro_load_game` succeeds and `game_loaded` is set; AFTER that, add:

```cpp
    if (s->game_loaded && s->hw_requested) {
        rp_av_info av{}; sh_get_av_info(core, &av);     // reuse the shim's av-info query for max geometry
        uint32_t mw = av.max_width ? av.max_width : av.base_width;
        uint32_t mh = av.max_height ? av.max_height : av.base_height;
        s->hw = std::make_unique<rp::HwRenderGL>();
        std::string e;
        if (!s->hw->setup(s->hw_cb.depth, s->hw_cb.stencil, s->hw_cb.bottom_left_origin,
                          mw, mh, (int)s->hw_cb.version_major, (int)s->hw_cb.version_minor, e)) {
            s->hw.reset(); s->game_loaded = false;
            return RP_ERR_DEVICE;                        // HW core can't run without its GL context
        }
        s->hw->make_current();
        if (s->hw_cb.context_reset) s->hw_cb.context_reset();   // core builds its GL objects
    }
    return RP_OK;   // (keep the existing successful return; fold the block in before it)
```

- [ ] **Step 6: `sh_run_frame` — make the GL context current before `retro_run`:**

```cpp
void sh_run_frame(rp_core* core) {
    auto* s = /* existing cast */;
    if (s->game_loaded) {
        if (s->hw) s->hw->make_current();   // HW cores render into our FBO under this context
        s->retro_run();
    }
}
```

- [ ] **Step 7: CMake** (`cores/libretro_shim/CMakeLists.txt`) — compile the GL sources into the shim + link
  system GL:

```cmake
add_library(LibretroShim SHARED
  LibretroShim.cpp
  HwRenderGL.cpp
  ${CMAKE_SOURCE_DIR}/src/render/gl/GLContext.cpp)
target_include_directories(LibretroShim PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/external/libretro ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(LibretroShim PRIVATE retropark_libretro_convert opengl32 gdi32)
```

  (Keep the existing POST_BUILD copy block. `${CMAKE_SOURCE_DIR}/src` on the include path makes
  `#include "render/gl/GLContext.h"` resolve.)

- [ ] **Step 8: Build + regression** — `cmake --build build --config Release`. Then confirm the **NES SW path
  is unregressed**: `./build/tests/Release/retropark_tests.exe` full suite green, and the gated FCEUmm test
  still passes if run (`RP_RUN_*` per `test_libretro_e2e`). The shim now links GL but a SW core never touches it.
- [ ] **Step 9: Commit** (`feat(shim): desktop-GL HW-render (readback) — accept SET_HW_RENDER, FBO, context_reset`).

---

### Task 4: N64 proof — Mupen64Plus-Next renders through the shim (gated e2e + harness)

**Files:**
- Create: `tests/test_hwrender_n64_e2e.cpp` (add to `retropark_tests`)
- Modify: `cores/libretro_shim/CMakeLists.txt` (stage the N64 core beside the shim if present)

- [ ] **Step 1: Download the core** (git-ignored, like FCEUmm) to
  `external/libretro-cores/mupen64plus_next_libretro.dll` from the official libretro buildbot
  (`https://buildbot.libretro.com/nightly/windows/x86_64/latest/mupen64plus_next_libretro.dll.zip` → unzip).
  NEVER commit it. Confirm it exists + is a PE DLL.
- [ ] **Step 2: Stage it beside the shim** — add to `cores/libretro_shim/CMakeLists.txt` a copy block mirroring
  the fceumm one:

```cmake
if (EXISTS ${CMAKE_SOURCE_DIR}/external/libretro-cores/mupen64plus_next_libretro.dll)
  add_custom_command(TARGET LibretroShim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/external/libretro-cores/mupen64plus_next_libretro.dll ${SHIM_OUT}/)
endif()
```

- [ ] **Step 3: The gated e2e** (`tests/test_hwrender_n64_e2e.cpp`), gated on `RP_RUN_N64=1` +
  `GLContext::probe()`-skip + core/ROM existence. It mirrors `test_libretro_e2e` but: (a) creates the runtime
  on the **D3D11 host** (`RP_GFX_D3D11`) so the shim's GL context is independent; (b) the shim must load
  `mupen64plus_next` (not fceumm) — set up a shim dir whose loaded core is the N64 DLL (copy the shim +
  `mupen64plus_next_libretro.dll` + a core.json naming it into a temp core dir, OR point the shim's core
  selection at it — inspect how `test_libretro_e2e` picks fceumm and do the N64 equivalent). ROM: an N64 file
  under `C:/RetroBat/roms/n64` (e.g. an Ocarina/Majora `.z64/.n64`). Assert the readback frame is **non-black
  and advancing** (frame N ≠ frame N+K) after pumping enough frames for the core to boot past its logo.

- [ ] **Step 4: RUN IT (the real validation).** `RP_RUN_N64=1 ./build/tests/Release/retropark_tests.exe
  -tc="*n64*"`. This is where HW-render is proven. **Likely failure points to debug (do NOT stub around them):**
  - Core requests a GL version/profile GLContext can't give → check `version_major/minor`; GLContext now honors
    them (Task 1). GLideN64 targets 3.3 core, which should work.
  - `context_reset` crash / no FBO → confirm `get_current_framebuffer` returns a complete FBO and the context
    is current before `context_reset`.
  - Core needs core-options set to select the GL rasterizer (Mupen64Plus-Next: `mupen64plus-rdp-plugin` =
    GLideN64) → the shim answers GET_VARIABLE with each option's DEFAULT (already implemented); the default RDP
    is GLideN64, so it should pick GL. If it picks angrylion (SW), the readback still works but confirm it went
    GL by logging the SET_HW_RENDER acceptance.
  - Black frames → dump one readback to a `.rgba`/PNG and inspect; verify the flip (Task 2 proved it) and that
    the core actually rendered (non-zero pixels in the FBO before flip).
  Fix real issues in `HwRenderGL`/the shim; the test passes only on genuine non-black advancing N64 frames.
- [ ] **Step 5: Harness capture** — `retropark_harness.exe --content <n64 rom>` with the shim configured for
  Mupen64Plus-Next (Release). Capture the window (PrintWindow→PNG) and confirm a real N64 game renders
  **upright**. Save the PNG for the human proof.
- [ ] **Step 6: Commit** (`test(shim): Mupen64Plus-Next N64 renders through GL HW-render readback (gated)`).

> **Slice close:** full suite green (incl. Task 1-2 GL tests on this GPU; the N64 e2e skips without the gate),
> then merge to `main` + push (no AI attribution). B2 (zero-copy GL-to-GL) is a separate future spec.

---

## Self-Review notes

- **Spec coverage:** GLFns/version (Task 1) / HwRenderGL FBO+readback (Task 2) / shim hooks (Task 3) / N64
  proof (Task 4) — every spec section maps to a task.
- **SW path untouched:** the `video_cb` HW branch is gated on the `RETRO_HW_FRAME_BUFFER_VALID` sentinel; a SW
  core never sets `hw_requested`, so `s->hw` stays null and the dupe/`convert_to_rgba8` paths are byte-identical.
- **No ABI change:** the runtime sees CPU RGBA via `video_refresh`, exactly as for a SW driven core.
- **Flip is proven independently** (Task 2's two-tone test) before the N64 frame depends on it — a solid-color
  or position-agnostic check would miss an inverted image.
- **Type consistency:** `get_current_framebuffer` returns `uintptr_t` (libretro `retro_hw_get_current_framebuffer_t`);
  `get_proc_address` returns `retro_proc_address_t`. `HwRenderGL::read_frame` returns a buffer valid until the
  next call — the shim forwards it synchronously inside `video_cb`, so no lifetime issue.
- **Task 4 is the risk.** It downloads a real core and must render real N64 frames; the plan lists concrete
  failure modes to debug rather than stub. If GLideN64 needs a GL feature GLContext lacks, that surfaces here.
