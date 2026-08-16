# B2 — Zero-copy GL-to-GL for HW-render cores — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** On the OpenGL host, a HW-render core's frame reaches the compositor as a GL texture (shared context),
not a CPU copy — removing B1's double readback. Two additive ABI hooks; the shim shares the host's GL context
or falls back to B1 readback. Proven in RetroPark (harness + gated N64 with a no-readback counter + D3D11
fallback), then shipped to EverythingBox.

**Architecture:** `HwRenderGL` shares the host's `HGLRC` at setup and hands the FBO color texture back via
`video_refresh_gl`; `GLBackend.composite_external_gl` draws the shared texture directly (origin-correct). Non-GL
hosts stay on B1. ABI 7→8.

**Tech Stack:** C++17, A's WGL `GLContext` + `GLCompositor`, the B1 shim, libretro HW-render, doctest, worktree deploy.

**Reference spec:** `docs/superpowers/specs/2026-08-15-retropark-b2-zerocopy-gl-design.md`.

## Global Constraints

- **D3D11/Vulkan hosts byte-unchanged.** `gl_share_context` returns null off-GL → the shim stays on B1 readback
  → the existing `composite_driven` CPU path. The GL-frame present branch only fires when a core called
  `video_refresh_gl`.
- **Share-failure degrades to B1**, never a broken load (log + fall back).
- **All cores recompile at ABI v8** (strict gate); the **Dolphin vehicle DLL must rebuild to v8** or its gated
  tests won't load.
- No AI attribution. RetroPark: merge to main + push at each task end. EB: worktree off origin/main.

---

### Task 1: GL machinery — context sharing, compositor `flipV`, `GLBackend` external-texture composite

**Files:** `src/render/gl/GLContext.{h,cpp}`, `GLCompositor.{h,cpp}`, `GLShaders.h`, `IRenderBackend.h`,
`src/render/gl/GLBackend.{h,cpp}`; Test: `tests/test_gl_external.cpp`. (No ABI change yet — builds at v7.)

- [ ] **Step 1: `GLContext` share + getter.** `GLContext.h`: change `initialize` to
  `bool initialize(void* native_window, uint32_t w, uint32_t h, std::string& err, int major=3, int minor=3, void* share_context=nullptr);`
  and add `void* hglrc() const { return hglrc_; }`. In `GLContext.cpp`, pass the share context to the ARB call:
  `hglrc_ = createAttribs(hdc_, (HGLRC)share_context, ctxAttribs);` (was `nullptr`). Existing callers (A backend,
  HwRenderGL B1) pass no share arg → unchanged.

- [ ] **Step 2: Compositor `flipV`.** `GLShaders.h` vertex shader: make the V-flip conditional on a uniform —
  `"uniform int uFlipV;\n"` and `"  vUV = vec2(p.x, uFlipV != 0 ? 1.0 - p.y : p.y);\n"`. `GLCompositor.h`:
  `void draw(const GLFns& g, GLuint tex, int flipV = 1);` (default 1 = current top-origin behavior). `.cpp`:
  cache `uFlipV_ = g.GetUniformLocation(prog_, "uFlipV");` in `initialize`, and in `draw` set
  `if (uFlipV_>=0) g.Uniform1i(uFlipV_, flipV);` before `DrawArrays`. (`GLBackend::composite_driven`'s existing
  `comp_.draw(g, frame_tex_)` now passes flipV=1 implicitly — unchanged.)

- [ ] **Step 3: `IRenderBackend` defaults.** In `IRenderBackend.h` add:

```cpp
    // GL zero-copy (B2). The host's GL context handle to share with (GL backend only), and compositing a core-
    // supplied external GL texture directly. Non-GL backends: no context / unsupported.
    virtual void* gl_context() const { return nullptr; }
    virtual rp_result composite_external_gl(unsigned /*tex*/, uint32_t /*w*/, uint32_t /*h*/,
                                            bool /*bottom_left_origin*/, uint8_t* /*out_rgba*/, std::string& err) {
        err = "backend has no GL external-texture composite"; return RP_ERR_UNSUPPORTED;
    }
```

- [ ] **Step 4: `GLBackend` overrides.** `GLBackend.h`: declare `void* gl_context() const override;` and
  `rp_result composite_external_gl(...) override;`. `.cpp`:

```cpp
void* GLBackend::gl_context() const { return ctx_.hglrc(); }

rp_result GLBackend::composite_external_gl(unsigned tex, uint32_t w, uint32_t h,
                                           bool bottom_left_origin, uint8_t* out_rgba, std::string& err) {
    if (!ready_) { err="not initialized"; return RP_ERR_DEVICE; }
    if (!ctx_.make_current()) { err="make_current"; return RP_ERR_DEVICE; }
    const GLFns& g=ctx_.gl();
    g.BindFramebuffer(GL_FRAMEBUFFER, headless_?fbo_:0);
    GLsizei vw=(GLsizei)width_, vh=(GLsizei)height_;
    if (!headless_) { uint32_t cw=0,ch=0; ctx_.client_size(cw,ch); if (cw&&ch){ vw=(GLsizei)cw; vh=(GLsizei)ch; } }
    g.Viewport(0,0,vw,vh);
    g.ClearColor(0,0,0,1); g.Clear(GL_COLOR_BUFFER_BIT);
    // CPU-upload frames are top-origin -> flipV=1 (see composite_driven). An external HW-render FBO texture is
    // GL bottom-origin -> the OPPOSITE flip. So flipV = bottom_left_origin ? 0 : 1. (Pinned by the flip test.)
    comp_.draw(g, (GLuint)tex, bottom_left_origin ? 0 : 1);
    (void)w; (void)h;   // the texture carries its own size; we composite into the host surface (width_/height_)
    if (headless_ && out_rgba) {
        g.PixelStorei(GL_PACK_ALIGNMENT,1);
        std::vector<uint8_t> flip((size_t)width_*height_*4);
        g.ReadPixels(0,0,(GLsizei)width_,(GLsizei)height_,GL_RGBA,GL_UBYTE,flip.data());
        const size_t row=(size_t)width_*4;
        for (uint32_t y=0;y<height_;++y)
            memcpy(out_rgba+(size_t)y*row, flip.data()+(size_t)(height_-1-y)*row, row);
    } else {
        ctx_.present();
    }
    return RP_OK;
}
```

- [ ] **Step 5: Flip test** (`tests/test_gl_external.cpp`, add to `retropark_tests`): create a `GLBackend`
  (headless), then a SECOND `GLContext` **sharing** with `backend.gl_context()`; in the shared context make a
  two-tone texture (top half green, bottom half red, painted so it mimics a bottom-origin HW-render FBO — reuse
  the `HwRenderGL::test_fill` idea or a direct `TexImage2D` of a known two-tone buffer). Call
  `backend.composite_external_gl(tex, w, h, /*bottom_left_origin*/true, out.data(), err)` and assert `out` is
  **upright** (row 0 = the visual top color) — proving the external-texture path + the origin flip. (Gate on
  `GLBackend::probe_gl_shared()`.) If it comes out inverted, flip the `bottom_left_origin ? 0 : 1` — the test is
  the arbiter.

- [ ] **Step 6: Build + run** → flip test passes; full suite green (A backend + compositor still build).
- [ ] **Step 7: Commit** (`feat(gl): shared-context creation + external-texture composite (B2 machinery)`).

---

### Task 2: ABI v8 + Runtime plumbing (trampolines, GL-frame present routing, stat)

**Files:** `include/retropark/retropark_abi.h`; `src/runtime/Runtime.{h,cpp}`; `include/retropark/retropark.h`;
`cores/*/core.json` (presenting cores → 8). Then rebuild the Dolphin vehicle to v8.

- [ ] **Step 1: ABI v8.** In `retropark_abi.h`, bump `RETROPARK_ABI_VERSION` 7→8 (update the comment) and add
  the two trailing `rp_host_iface` fields (after `audio_want`):

```c
    void* (*gl_share_context)(rp_host* host);
    void  (*video_refresh_gl)(rp_host* host, unsigned gl_texture, uint32_t width, uint32_t height, int bottom_left_origin);
```

  Bump `cores/dolphin_present/core.json` + `cores/rpcs3_present/core.json` `abi_version` to 8 (cosmetic; the
  loader reads the compiled struct).

- [ ] **Step 2: Runtime trampolines + wiring.** In `Runtime.cpp` add:

```cpp
static void* host_gl_share_context(rp_host* h) { return reinterpret_cast<Runtime*>(h)->on_gl_share_context(); }
static void host_video_refresh_gl(rp_host* h, unsigned tex, uint32_t w, uint32_t hh, int blo) {
    reinterpret_cast<Runtime*>(h)->on_video_refresh_gl(tex, w, hh, blo);
}
```

  Wire in the ctor: `host_iface_.gl_share_context = host_gl_share_context; host_iface_.video_refresh_gl = host_video_refresh_gl;`

- [ ] **Step 3: Runtime handlers + state.** `Runtime.h`: add `void* on_gl_share_context();`,
  `void on_video_refresh_gl(unsigned tex, uint32_t w, uint32_t h, int blo);`, getters
  `uint64_t gl_frames() const`, and state `unsigned dr_gl_tex_=0; uint32_t dr_gl_w_=0, dr_gl_h_=0; bool dr_gl_blo_=false; bool dr_is_gl_=false; std::atomic<uint64_t> gl_frames_{0};`. `Runtime.cpp`:

```cpp
void* Runtime::on_gl_share_context() { return backend_ ? backend_->gl_context() : nullptr; }
void Runtime::on_video_refresh_gl(unsigned tex, uint32_t w, uint32_t h, int blo) {
    dr_have_ = true; dr_dupe_ = false; dr_is_gl_ = true;
    dr_gl_tex_ = tex; dr_gl_w_ = w; dr_gl_h_ = h; dr_gl_blo_ = (blo != 0);
    gl_frames_.fetch_add(1, std::memory_order_relaxed);
}
```

  In `on_video_refresh` (the CPU path) set `dr_is_gl_ = false;` so a later CPU frame clears the GL flag. In the
  reset/unload spots that clear `dr_data_`/`dr_have_` (Runtime.cpp:242, :315), also clear `dr_is_gl_ = false; dr_gl_tex_ = 0;`.

- [ ] **Step 4: Present routing.** At the driven composite (Runtime.cpp ~:342-345), branch on `dr_is_gl_`:

```cpp
    if (dr_is_gl_ && dr_have_) {
        return backend_->composite_external_gl(dr_gl_tex_, dr_gl_w_, dr_gl_h_, dr_gl_blo_, out_rgba, err);
    }
    bool valid = dr_have_ && !dr_dupe_ && /* existing geometry checks */;
    return backend_->composite_driven(valid ? dr_data_ : nullptr, /* existing args */);
```

- [ ] **Step 5: Stat C API.** `retropark.h` + `Runtime.cpp`: `uint64_t rp_runtime_gl_frame_count(rp_runtime* rt)`
  → `gl_frames()` (the no-readback proof for the N64 test: >0 means zero-copy engaged).

- [ ] **Step 6: Build host + cores (v8).** `cmake --build build --config Release`. Then **rebuild the Dolphin
  vehicle to v8** (MSBuild `RetroParkDolphin.vcxproj`, per docs/dolphin-build.md) so its `rp_get_core_abi`
  reports 8 — else the gated Dolphin tests fail to load. Full suite green; the Dolphin gated e2e (if run) still
  loads.
- [ ] **Step 7: Commit** (`feat(abi): v8 — gl_share_context + video_refresh_gl host hooks + Runtime GL-frame routing`).

---

### Task 3: Shim — share the host's GL context, hand the texture (zero-copy) or fall back to readback

**Files:** `cores/libretro_shim/HwRenderGL.{h,cpp}`, `LibretroShim.cpp`. Rebuild the shim.

- [ ] **Step 1: `HwRenderGL` share + color getter.** `HwRenderGL.h`: `setup` gains a trailing
  `void* share_context` param; add `unsigned color_texture() const { return color_; }` and `bool zero_copy() const { return zero_copy_; }` + member `bool zero_copy_=false;`. `.cpp` `setup`: pass `share_context` to
  `ctx_.initialize(nullptr, maxW_, maxH_, err, major, minor, share_context)` and set `zero_copy_ = (share_context != nullptr);`. **If the shared-context init fails AND share_context was non-null, retry once WITHOUT sharing** (standalone) and set `zero_copy_ = false` (degrade to B1, don't fail the load).

- [ ] **Step 2: Shim wires the share + zero-copy video path.** In `LibretroShim.cpp` `sh_load_content`'s HW-setup
  block, query the host BEFORE `setup` and pass it:

```cpp
    void* share = (s->host.gl_share_context) ? s->host.gl_share_context(s->host.host) : nullptr;
    if (!s->hw->setup(s->hw_cb.depth, s->hw_cb.stencil, s->hw_cb.bottom_left_origin,
                      mw, mh, (int)s->hw_cb.version_major, (int)s->hw_cb.version_minor, share, e)) { /* existing fail path */ }
```

  In `video_cb`, the `RETRO_HW_FRAME_BUFFER_VALID` branch:

```cpp
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        if (g->hw && g->hw->zero_copy() && g->host.video_refresh_gl) {           // B2: hand the GL texture
            g->host.video_refresh_gl(g->host.host, g->hw->color_texture(), w, h, /*bottom_left_origin*/1);
        } else {                                                                  // B1: readback
            uint32_t cw=w, ch=h, p=0;
            const void* rgba = g->hw ? g->hw->read_frame(cw, ch, p) : nullptr;
            if (rgba) g->host.video_refresh(g->host.host, rgba, cw, ch, p);
            else      g->host.video_refresh(g->host.host, nullptr, w, h, 0);
        }
        return;
    }
```

- [ ] **Step 3: Build the shim + rebuild the N64 shim dir** (`--target LibretroShim` restages
  `build/cores/libretro_shim_n64`). Full suite green; the SW/NES path unregressed (no HW callback → no share, no
  change).
- [ ] **Step 4: Commit** (`feat(shim): B2 zero-copy — share the host GL context, hand the FBO texture`).

---

### Task 4: N64 zero-copy proof (GL host) + no-readback counter + D3D11 fallback

**Files:** extend `tests/test_hwrender_n64_e2e.cpp`.

- [ ] **Step 1: GL-host zero-copy case.** Add a gated case (or parameterize the existing N64 e2e over host api):
  create the runtime on **`RP_GFX_OPENGL`** (probe-skip without GL), load the N64 shim + Banjo-Tooie, pump, and
  assert (a) non-black + advancing (renders via zero-copy) **and (b) `rp_runtime_gl_frame_count(rt) > 0`** — the
  shim handed GL textures, i.e. zero-copy engaged (no CPU readback). Save a frame for the human check.
- [ ] **Step 2: D3D11 fallback case.** The existing N64 e2e on `RP_GFX_D3D11` must still pass AND
  `rp_runtime_gl_frame_count(rt) == 0` (fell back to B1 readback — no GL frames). This proves the fallback +
  that D3D11 is untouched.
- [ ] **Step 3: Run** both gated (`RP_RUN_N64=1`): GL-host renders with gl_frame_count>0; D3D11 renders with
  gl_frame_count==0. Convert the GL-host frame to PNG and eyeball Banjo-Tooie (upright). Windowed harness
  `--api gl --content <n64 rom>` for the full-zero-copy visual.
- [ ] **Step 4: Commit** (`test: N64 zero-copy on the GL host (no-readback) + D3D11 B1 fallback (gated)`).

> **RetroPark slice close:** full suite green; merge to main + push. Task 5 is EverythingBox.

---

### Task 5: EverythingBox — bump to v8 + rebuild (incl. Dolphin vehicle) + deploy + verify

**Repo:** EverythingBox worktree off origin/main.

- [ ] **Step 1: Worktree + submodule bump** to the Task-4 RetroPark commit. Wipe `build/retropark_ext-prefix`
  (submodule source + ABI changed). **The Dolphin vehicle is git-ignored and staged from
  `EB_DOLPHIN_VEHICLE_DIR`** — ensure that dir's `dolphin_present.dll` is the **v8** rebuild (Task 2 Step 6),
  else EB's Dolphin (gc) load fails the v8 gate. (Confirm the vehicle DLL's abi_version is 8.)
- [ ] **Step 2: Build Release** (worktree). retropark.lib + shim recompile at v8; the vehicle DLL (v8) restages.
  0 errors, EverythingBox.exe relinks.
- [ ] **Step 3: Deploy** — targeted-copy `EverythingBox.exe` (+`.pdb`) AND the **v8 `dolphin_present.dll`**
  (`cores/dolphin_present/`) + its `core.json` to `C:\EverythingBox-app` (the vehicle DLL changed with the ABI
  bump — NOT just the exe this time). No `/MIR`.
- [ ] **Step 4: Verify** — N64 on the **OpenGL driven backend** renders in-app (zero-copy); N64 on D3D11 still
  renders (B1); GameCube (Dolphin, v8 vehicle) still renders; NES unaffected. (If the in-app N64 launch can't be
  auto-driven, the RetroPark-level zero-copy proof + a healthy deployed app stand, with the hands-on check per
  the N64 increment.)
- [ ] **Step 5:** Update memory ([[retropark-project]] / [[retropark-eb-integration]]) with B2 done.

---

## Self-Review notes

- **Spec coverage:** GL machinery (T1) / ABI+Runtime (T2) / shim (T3) / N64 proof (T4) / EB (T5) — all mapped.
- **D3D11/Vulkan byte-unchanged:** `dr_is_gl_` is only set by `on_video_refresh_gl`, only called after a non-null
  `gl_share_context`, only returned by the GL backend; the CPU present branch is otherwise identical.
- **ABI v8 forces the Dolphin vehicle rebuild** — called out in T2 (tests) and T5 (deploy); the vehicle DLL ships
  in the T5 deploy, unlike the exe-only N64 deploy.
- **Origin flip** is pinned by T1's two-tone test before the N64 frame depends on it (the B1 lesson).
- **Share-failure degrades to B1** (T3 Step 1 retry-without-sharing) — never a broken load.
- **Type consistency:** `gl_share_context`→`backend_->gl_context()`→`GLContext::hglrc()`; `video_refresh_gl` tex →
  `dr_gl_tex_` → `composite_external_gl` → `comp_.draw(tex, flipV)`; the no-readback proof is `gl_frames_` (>0 on
  GL host, 0 on D3D11).
