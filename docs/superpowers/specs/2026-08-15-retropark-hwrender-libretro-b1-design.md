# RetroPark HW-Render libretro Cores — Design (Subsystem B, increment B1: readback)

**Status:** design, approved 2026-08-15. Implementation pending (writing-plans next).

## Context & decomposition

This is **Subsystem B** of "add OpenGL to RetroPark" (Subsystem A, the OpenGL host compositor backend, is
DONE — commits `16d425d..ac83def`). B = **hardware-rendered libretro cores** (N64/PSP/Dreamcast/PS1-HW), which
render via a GL context the frontend provides instead of handing back CPU pixels. B is itself decomposed:

- **B1 (THIS spec): the readback path.** The libretro shim implements GL HW-render, the core renders into a
  frontend-provided FBO, and the shim reads that FBO back to CPU RGBA and forwards it through the EXISTING
  driven `video_refresh` path. Works with ANY host backend (D3D11/Vulkan/GL). Gets HW cores RENDERING first.
- **B2 (LATER, its own spec): zero-copy GL-to-GL.** An optimization on the OpenGL host — the core renders into
  a shared texture composited directly (a new `video_refresh_gl` host hook, ABI bump), no per-frame readback.
  Out of scope here.

B builds on A: B1 reuses A's `GLContext` (the hand-rolled WGL 3.3-core context) to create the GL context it
hands the core. B1 does NOT require A's GL host *backend* — the readback goes through the driven path, so any
host composites it.

## Goal

The libretro shim (`cores/libretro_shim`) implements the GL hardware-render callback so a real HW-render core —
**Mupen64Plus-Next (N64)** as the proof — renders into a shim-provided GL FBO; the shim reads it back to CPU
RGBA (row-flipped) and forwards it through the unchanged driven `video_refresh` path, so any host backend
composites it. Proven by a gated N64 e2e (non-black + advancing) and a windowed harness capture. The NES/SW
path stays unregressed. RetroPark repo only (no EverythingBox, no deploy).

## Non-goals (YAGNI / scope bounds)

- **Desktop GL only.** Accept `RETRO_HW_CONTEXT_OPENGL` / `RETRO_HW_CONTEXT_OPENGL_CORE` (Mupen64Plus-Next
  requests OPENGL_CORE 3.3, which A's `GLContext` already creates). GLES contexts (via ANGLE) are rejected and
  deferred — a core requesting GLES either falls back to SW or fails to load; that is acceptable for B1.
- **Readback path only.** Zero-copy GL-to-GL is B2.
- **No EverythingBox integration, no deploy.** B1 stops at the RetroPark repo.
- **`context_reset` once at start.** No mid-run GL context loss / resize / `cache_context=false` re-reset
  handling — the context is created once before the first `retro_run` and lives for the core's lifetime.
- **N64/Mupen64Plus-Next is the validation target**, but the mechanism is core-agnostic (any desktop-GL
  HW-render libretro core routes through the same path).
- **No non-GL HW APIs** (Vulkan/D3D/Metal HW-render contexts) — rejected.

## Approach

**Extend the existing shim in place with an isolated GL module.** The shim already loads any libretro core and
forwards SW frames (`PixelConvert`); B1 adds a `HwRenderGL` unit that owns everything GL, keeping the shim's
core-loading logic clean. Rejected alternatives: a separate HW-only shim core (duplicates the libretro
loading/env boilerplate for no gain) and borrowing the GL *host backend's* context (couples B1 to the OpenGL
host and buys nothing in a readback model).

## Architecture

New + modified files, all under `cores/libretro_shim/` except the shared GL extension:

- **`cores/libretro_shim/HwRenderGL.{h,cpp}` (new)** — owns the GL side, isolated from shim logic:
  - Holds a `rp::GLContext` (from `src/render/gl/`, compiled into the shim — it is self-contained WGL), a
    framebuffer object (color texture `RGBA8` + a depth or depth-stencil renderbuffer), and a CPU readback
    buffer.
  - `bool setup(bool depth, bool stencil, bool bottom_left_origin, uint32_t maxW, uint32_t maxH, std::string& err)`
    — create the headless GL 3.3-core context, allocate the FBO at `maxW×maxH` (the core's `max_width/height`;
    the core renders at up to this and reports the actual `w×h` per frame), attach color + (if requested)
    depth/-stencil, check completeness.
  - `unsigned fbo_id() const` — the GL FBO name, returned from `get_current_framebuffer`.
  - `bool make_current()` — make the GL context current (called at the top of each `retro_run`).
  - `const void* read_frame(uint32_t w, uint32_t h, uint32_t& out_pitch)` — bind the FBO, `glReadPixels` the
    `w×h` region, row-flip into the readback buffer iff `bottom_left_origin` (GL default; a core setting
    `bottom_left_origin=false` means top-origin, no flip), return the top-origin RGBA buffer + `w*4` pitch.
  - Teardown in the dtor (FBO/textures/renderbuffer die with the context).
- **`src/render/gl/GLContext.{h,cpp}` (extend, additive)** — add 4 renderbuffer entrypoints to `GLFns`
  (`GenRenderbuffers`, `BindRenderbuffer`, `RenderbufferStorage`, `FramebufferRenderbuffer`) and load them in
  `initialize`, for the FBO's depth attachment. Additive — the GL host backend (A) is unaffected.
- **`cores/libretro_shim/LibretroShim.cpp` (modify)** — the HW-render hooks:
  - **`RETRO_ENVIRONMENT_SET_HW_RENDER`**: inspect the `retro_hw_render_callback`. If `context_type` is
    `OPENGL` or `OPENGL_CORE`, accept (return true): store the callback, and fill in `get_current_framebuffer`
    (returns `HwRenderGL::fbo_id`) and `get_proc_address` (a wrapper over `wglGetProcAddress` with the
    `GetProcAddress(opengl32.dll)` fallback, so the core loads its OWN GL functions). Any other `context_type`
    (GLES, Vulkan, D3D, none) → return false (rejected).
  - **After `retro_load_game`** (where the core has done `SET_HW_RENDER` + reported `av_info`): if a HW callback
    was stored, `HwRenderGL::setup(depth, stencil, bottom_left_origin, av.max_width, av.max_height)` then call
    the core's `context_reset()` (context current). If setup fails, fail the load (`RP_ERR_DEVICE`).
  - **`retro_run` wrapper** (the shim's per-frame `run_frame`): if HW mode, `HwRenderGL::make_current()` before
    `retro_run`. The core renders into the FBO and calls `video_refresh` with the `RETRO_HW_FRAME_BUFFER_VALID`
    sentinel (`data == (void*)-1`) + `w,h`. The shim's `video_cb`, on that sentinel, calls
    `HwRenderGL::read_frame(w,h)` and forwards the resulting CPU RGBA to the real host `video_refresh`
    (`g->host.video_refresh(host, rgba, w, h, w*4)`) — exactly like the SW path's output, so the runtime is
    unchanged.
  - **`RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY` / `GET_SAVE_DIRECTORY`**: return a writable dir beside the shim
    (N64 cores expect these for BIOS-less config/saves; harmless for SW cores).
  - The SW path is byte-unchanged: a core that never calls `SET_HW_RENDER` has no stored HW callback, so
    `video_cb` runs the existing `PixelConvert` branch.
- **`cores/libretro_shim/CMakeLists.txt` (modify)** — add `src/render/gl/GLContext.cpp` + `HwRenderGL.cpp` to
  the shim's sources; link `opengl32`, `gdi32`.

## Data flow (one HW-render frame)

```
load_content
  -> retro_load_game
       -> core: SET_HW_RENDER(OPENGL_CORE, v3.3, depth=true)   -> shim accepts, stores cb, sets fb/proc getters
  -> shim: HwRenderGL.setup(depth,stencil,blo, av.max_w, av.max_h)   (headless GL 3.3 ctx + FBO)
  -> shim: core.context_reset()                                       (core builds its GL objects)
run_frame (per frame):
  -> HwRenderGL.make_current()
  -> retro_run()
       -> core: get_current_framebuffer() -> our FBO id; renders into it
       -> core: video_refresh(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0)
            -> shim: HwRenderGL.read_frame(w,h) [glReadPixels + row-flip] -> host.video_refresh(rgba,w,h,w*4)
  -> runtime uploads CPU RGBA -> host composites (unchanged driven path, any backend)
```

## Error handling

- Core requests a non-GL or GLES HW context → `SET_HW_RENDER` returns false; the core falls back to SW or
  fails to boot (core-dependent). Documented limitation, not a crash.
- No GL 3.3 context available → `HwRenderGL::setup` fails → `load_content` returns `RP_ERR_DEVICE` with a clear
  `err`. A probe (`GLContext::probe()`) gate-skips the N64 test on a GL-less machine.
- Incomplete FBO → setup error.
- A core that renders nothing / a black frame → the readback is black; the gated test's non-black assertion
  fails (so a silently-broken HW path is caught).
- SW/NES cores: never reach the HW path (no `SET_HW_RENDER`) → unaffected.

## Testing

- **`tests/test_hwrender_n64_e2e.cpp` (new), gated `RP_RUN_N64=1`** (and `GLContext::probe()`-skipped): point a
  shim instance at **Mupen64Plus-Next** (a git-ignored downloaded core `mupen64plus_next_libretro.dll`, staged
  beside the shim like fceumm) + a real N64 ROM (e.g. `C:/RetroBat/roms/n64/…Zelda…`). Create the runtime on
  the **D3D11 host** (keeps the shim's GL context independent of any host GL context) → `load_core(shim)` +
  `load_content(rom)` → pump `rp_runtime_present` → assert the readback frame is **non-black and advancing**
  (frame N ≠ frame N+K). Skips cleanly with the gate unset or the core/ROM absent.
- **Windowed harness**: `--content <n64 rom>` with the shim configured for Mupen64Plus-Next → visual capture
  (real N64 game upright in the window).
- **NES/SW unregressed**: the existing `test_libretro_e2e` (FCEUmm) and the full suite stay green — the SW path
  is untouched.

## Dependencies / assets

- **Mupen64Plus-Next** libretro core DLL — downloaded from the official buildbot to the git-ignored
  `external/libretro-cores/` (same pattern as FCEUmm), never committed. GLideN64 is compiled into it; it
  requests `RETRO_HW_CONTEXT_OPENGL_CORE` 3.3 and depth.
- N64 ROMs at `C:/RetroBat/roms/n64` (present: 366 ROMs incl. Ocarina/Majora's Mask).
- `external/libretro/libretro.h` (already vendored) — the `retro_hw_render_callback`,
  `RETRO_HW_FRAME_BUFFER_VALID`, `RETRO_HW_CONTEXT_*`, `get_current_framebuffer`, `get_proc_address` defs.

## File-structure summary

| File | Responsibility |
|------|----------------|
| `cores/libretro_shim/HwRenderGL.{h,cpp}` | GL context + FBO (color+depth) + readback/flip for HW-render cores |
| `cores/libretro_shim/LibretroShim.cpp` | SET_HW_RENDER accept, fb/proc getters, context_reset, HW video path, GET_*_DIRECTORY |
| `src/render/gl/GLContext.{h,cpp}` | +4 renderbuffer entrypoints in GLFns (additive) |
| `cores/libretro_shim/CMakeLists.txt` | compile GLContext.cpp + HwRenderGL.cpp; link opengl32/gdi32 |
| `tests/test_hwrender_n64_e2e.cpp` | gated Mupen64Plus-Next N64 readback e2e (non-black + advancing) |

## Open follow-ups (explicitly deferred)

- **B2**: zero-copy GL-to-GL (shared context + `video_refresh_gl` host hook, ABI bump) on the OpenGL host.
- GLES/ANGLE HW contexts (PPSSPP, Flycast GLES paths).
- EverythingBox integration + deploy for N64/HW-render (its own increment).
- Mid-run context reset / resize / `cache_context=false`.
- Other HW cores (Beetle-PSX-HW, ParaLLEl-N64, Flycast) — same mechanism, incremental validation.
