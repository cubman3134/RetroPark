# B2 — Zero-copy GL-to-GL for HW-render cores — Design

**Status:** design, approved 2026-08-15. Implementation pending (writing-plans next).

## Context

B1 (HW-render libretro cores, readback) is DONE: a GL core renders into the shim's FBO, the shim `glReadPixels`
it to CPU RGBA, and it composites through the driven path. On the OpenGL host (Subsystem A) that means a
**double readback** — the shim reads mupen's FBO to CPU, then the GL host re-uploads it to a texture and
composites. B2 removes that: the shim hands the GL host mupen's FBO **texture** directly (shared GL context),
so the host composites it with no shim readback and no re-upload. Falls back to B1 on non-GL hosts. Builds on A
(GL host) + B1 (HW shim). Spans RetroPark (the mechanism) + EverythingBox (submodule bump + deploy so N64 on
the OpenGL driven backend gets it).

## Goal

On the OpenGL host, a HW-render libretro core's rendered frame reaches the compositor as a **GL texture, not a
CPU copy** — the shim shares its GL context with the host and hands back the FBO texture via a new
`video_refresh_gl` hook; the GL compositor draws it directly. Windowed = full zero-copy (no readback anywhere);
headless/EB = the shim's readback + re-upload eliminated (the host's final readback for QImage/out_rgba
remains, inherent to that path). On D3D11/Vulkan hosts the core stays on B1 readback, byte-unchanged. Proven
in RetroPark (harness + gated N64) and shipped to EverythingBox (N64 on the OpenGL driven backend).

## Non-goals (YAGNI)

- **Zero-copy only on the GL host.** D3D11/Vulkan → B1 readback (a GL↔D3D/Vulkan texture-interop path is not
  worth it here; the fallback is correct and already ships).
- **N64/Mupen64Plus-Next is the proof** (mechanism is core-agnostic).
- **No new EB UI.** B2 activates automatically when the user has selected the OpenGL driven backend (A's toggle)
  for an N64 game; no RetroParkView routing change beyond the submodule bump.
- No GLES; no B1 removal (readback stays as the fallback).

## Architecture

### ABI v7 → v8 (additive — 2 trailing `rp_host_iface` hooks)

```c
/* GL zero-copy (B2): the host's GL context handle (HGLRC on Windows) a HW-render core wglShareLists with, so it
   can hand back a GL texture instead of CPU pixels. NULL if the host is not OpenGL -> the core falls back to
   CPU readback via video_refresh. */
void* (*gl_share_context)(rp_host* host);
/* GL zero-copy (B2): hand the host a GL texture (name valid in the host's context via the shared context above)
   as this frame instead of CPU pixels. bottom_left_origin != 0 => the texture is GL bottom-origin (the usual
   HW-render case). Only called when gl_share_context returned non-NULL. */
void (*video_refresh_gl)(rp_host* host, unsigned gl_texture, uint32_t width, uint32_t height, int bottom_left_origin);
```

`RETROPARK_ABI_VERSION` 7→8; all cores recompile (strict-equality gate). A core that doesn't use them (SW cores,
refcores) is unaffected — the fields are just null-unused for them.

### Components

- **`GLContext`** — `initialize` gains an optional `void* share_context` passed as the share arg to
  `wglCreateContextAttribsARB` (so the new context shares lists with the host's). A `void* hglrc()` getter.
- **`GLBackend`** — implements a new `IRenderBackend` accessor `void* gl_context() const` (returns
  `ctx_.hglrc()`; default null on other backends) so the Runtime can expose it. Adds
  **`composite_external_gl(unsigned tex, uint32_t w, uint32_t h, bool bottom_left_origin, uint8_t* out_rgba,
  std::string& err)`** — make current, bind the target framebuffer (headless FBO / windowed default), draw
  `tex` via the compositor with the correct V-flip for the texture's origin, then readback (headless) or
  `SwapBuffers` (windowed). The compositor's draw gains a `flipV` control (a `uFlipV` uniform, or the existing
  shader path): CPU-upload frames are top-origin (flip), external HW-render textures are bottom-origin (the
  opposite) — a two-tone test pins the exact combination.
- **`HwRenderGL` (shim)** — at `setup`, query `host.gl_share_context(host.host)`. **Non-null:** create the GL
  context *sharing* with it (`GLContext::initialize(..., share_hglrc)`) → **zero-copy mode**; expose the FBO
  color texture id (`color_texture()`). **Null:** the current standalone context → **B1 readback mode**
  (unchanged). Store the mode.
- **`LibretroShim.cpp` `video_cb`** — on the `RETRO_HW_FRAME_BUFFER_VALID` sentinel: zero-copy mode →
  `host.video_refresh_gl(host, hw->color_texture(), w, h, /*bottom_left_origin*/1)` (no readback); readback mode
  → the current `read_frame` → `video_refresh(CPU)`. (Guard `video_refresh_gl` non-null defensively; the v8 gate
  guarantees it when zero-copy is active.)
- **`Runtime`** — `gl_share_context` trampoline returns `backend_->gl_context()`; `on_video_refresh_gl(tex, w, h,
  blo)` stores a GL-frame descriptor (`dr_gl_tex_`, dims, origin, `dr_is_gl_`); `present` routes a GL frame to
  `backend_->composite_external_gl(...)` instead of `composite_driven(CPU)`. A driven core that never calls
  `video_refresh_gl` (SW, or HW on a non-GL host) takes the unchanged CPU path.

## Data flow (B2, GL host)

```
mupen renders into shim FBO color texture (shim GL ctx SHARES lists with the host's GL ctx)
  → shim: host.video_refresh_gl(fbo_color_tex, w, h, bottom_left_origin=1)   [NO glReadPixels, NO re-upload]
  → Runtime stores the GL frame; present() -> GLBackend.composite_external_gl(tex, ...)
       → compositor draws the shared texture into the host framebuffer (origin-correct)
       → headless: glReadPixels -> out_rgba (host's one readback, for QImage/EB)
         windowed:  SwapBuffers (FULL zero-copy, no readback anywhere)
```

Non-GL host: `gl_share_context` null → shim stays B1 (standalone ctx + readback + `video_refresh` CPU) → the
existing `composite_driven` path. Byte-unchanged.

## Error handling

- `wglShareLists`/shared-context creation fails (incompatible pixel formats, driver quirk) → `HwRenderGL::setup`
  **falls back to B1** (standalone context + readback) with a log, rather than failing the load. So a
  share-failure degrades to correct-but-slower, never a broken load.
- `gl_share_context` null (non-GL host) → B1 (the only path today).
- A GL frame on a non-GL backend can't happen (the shim only calls `video_refresh_gl` after a non-null
  `gl_share_context`, which only the GL backend returns).
- SW/refcores: never touch B2 (no HW callback).

## Testing

- **Two-tone origin test** (RetroPark unit-ish, GL): render a known two-tone texture in a shared context and
  assert `composite_external_gl` produces it upright in the readback — pins the flip (the B1 lesson: origin bugs
  hide behind position-agnostic tests).
- **Gated N64 on the GL host** (`RP_RUN_N64=1`, `RP_GFX_OPENGL`): Banjo-Tooie renders correctly via the
  zero-copy path (non-black + advancing) **and** a counter proves the shim did **no** CPU readback
  (`video_refresh_gl` was used, `read_frame`/`video_refresh` not) — e.g. an `rp_runtime` stat for gl-frame count
  vs cpu-frame count. **Fallback:** the same N64 test on the D3D11 host still renders (B1 readback path intact).
- **Windowed harness** `--api gl --content <n64 rom>` = full zero-copy visual capture (upright).
- Full suite green; SW/NES + refcore paths unregressed.

## EverythingBox + deploy

- Bump `external/RetroPark` submodule to the B2 commit (ABI v8 → retropark.lib + shim + all cores recompile;
  the Dolphin vehicle DLL must ALSO rebuild to v8 — it's a presenting core; stage the fresh DLL). No
  RetroParkView change: B2 activates automatically when the user selected the **OpenGL driven backend** (A's
  toggle) for N64 — the shim sees a GL host and shares. On the D3D11 default it stays B1. Worktree build +
  targeted redeploy (+ the v8 Dolphin vehicle DLL).
- **Verify:** N64 on the OpenGL driven backend renders in-app (zero-copy); N64 on D3D11 still renders (B1);
  NES + GameCube unaffected.

## File-structure summary

| File | Responsibility |
|------|----------------|
| `include/retropark/retropark_abi.h` | +`gl_share_context` + `video_refresh_gl` in rp_host_iface; ABI 7→8 |
| `src/render/gl/GLContext.{h,cpp}` | `initialize(..., share_context)`; `hglrc()` getter |
| `src/render/IRenderBackend.h` + `gl/GLBackend.{h,cpp}` | `gl_context()`; `composite_external_gl` |
| `src/render/gl/GLCompositor.{h,cpp}` + `GLShaders.h` | `flipV` control for external bottom-origin textures |
| `src/runtime/Runtime.{h,cpp}` | trampolines; `on_video_refresh_gl`; GL-frame present routing; gl-frame stat |
| `cores/libretro_shim/HwRenderGL.{h,cpp}` | share-or-standalone at setup; `color_texture()` |
| `cores/libretro_shim/LibretroShim.cpp` | `video_cb` zero-copy vs readback branch |
| `tests/test_hwrender_n64_e2e.cpp` + a flip test | GL-host zero-copy + no-readback + D3D11 fallback |
| EB submodule bump + deploy | v8 rebuild (incl. Dolphin vehicle) + verify |

## Open follow-ups (deferred)

- GLES/ANGLE HW cores; other HW systems (PS1-HW, Dreamcast, PSP).
- GL↔D3D/Vulkan texture interop (zero-copy under non-GL hosts) — not worth it; B1 fallback suffices.
- A true dmabuf/keyed-mutex cross-process handoff (RPCS3-style) — different arc.
