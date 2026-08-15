# RetroPark OpenGL Host Backend — Design (Subsystem A)

**Status:** design, approved 2026-08-15. Implementation pending (writing-plans next).

## Context & decomposition

The user asked to "add OpenGL to RetroPark." That is two independently-buildable subsystems:

- **Subsystem A (THIS spec): an OpenGL host compositor backend** — a third `IRenderBackend`
  (`RP_GFX_OPENGL`) beside `D3D11Backend`/`VulkanBackend`, compositing driven-core CPU frames via
  OpenGL, headless + windowed, wired into EverythingBox and deployed.
- **Subsystem B (LATER, its own spec): HW-render libretro cores** — the libretro shim implements
  `retro_hw_render_callback` so GL cores (N64/Mupen, PSP/PPSSPP, Dreamcast/Flycast, …) render into a
  host-provided GL FBO. B builds on A: A is the GL context/compositor that lets B's GL frames composite
  directly instead of reading back to CPU. B is out of scope here.

A is the foundation; it ships and is proven in-app before B starts.

## Goal

A third `IRenderBackend` — `GLBackend` + `GLCompositor` — that composites **driven-core** CPU RGBA frames
via **OpenGL 3.3 core**, headless (`glReadPixels`) and windowed (WGL `SwapBuffers`), registered in
`BackendFactory` as `RP_GFX_OPENGL`, proven with `refcore_driven` **and real NES** (FCEUmm software shim),
integrated into EverythingBox as a **user-selectable driven backend**, and deployed to `C:\EverythingBox-app`.

## Non-goals (YAGNI / scope bounds)

- **Presenting cores are excluded.** Dolphin/RPCS3 are Vulkan presenting cores and stay on the Vulkan host.
  Compositing a Vulkan core's shared image from a GL host is GL↔Vulkan interop that buys nothing. The GL
  backend's actual presenting-frame path (`composite_and_present`) returns `RP_ERR_UNSUPPORTED` — but see the
  `allocate_surfaces` note below: it must still SUCCEED, because the driven-core resize path routes through it.
- **Windows/WGL only.** GLX/EGL (Linux/Mac) is deferred behind the `GLContext` seam; the Linux/Mac port is a
  separate future effort (the D3D11 backend is Windows-only too — portability comes later everywhere).
- **No HW-render libretro cores** (Subsystem B).
- **No new third-party dependency.** No glad/GLEW; the compositor needs only ~20 GL entrypoints, loaded by
  hand via `wglGetProcAddress`, matching how D3D11 and Vulkan are hand-rolled here. Links only the system
  `opengl32.lib` + `gdi32.lib`.
- **No overlay.** The compositor draws only the core frame (the blue test overlay was removed brand-wide in
  `e735b88`).

## Architecture

New directory `src/render/gl/`, mirroring `src/render/vulkan/`:

- **`GLContext.{h,cpp}`** — the platform seam. ALL Win32/WGL lives here:
  - Create a GL 3.3-core context. **Headless:** a hidden 1×1 window + its `HDC`; a legacy context first, then
    `wglCreateContextAttribsARB` for a 3.3 core profile. **Windowed:** the same, from the caller's native
    `HWND`. (Uses the standard dummy-window bootstrap to obtain `wglCreateContextAttribsARB` /
    `wglChoosePixelFormatARB`.)
  - `make_current()`, `swap_buffers()` (windowed) / `flush()` (headless), `destroy()`.
  - Load the ~20 GL entrypoints the compositor needs via `wglGetProcAddress` (fallback
    `GetProcAddress(opengl32)` for 1.1 symbols) into a small function-pointer table.
  - `static bool probe_gl_shared()` — can a 3.3-core context be created on this machine? (Tests gate-skip on
    it, exactly like `VulkanBackend::probe_vulkan_shared()`.)
- **`GLBackend.{h,cpp}`** — implements `IRenderBackend`:
  - `initialize(native_window, w, h, err)` — build the context (headless if `native_window == nullptr`,
    windowed otherwise); create the offscreen FBO (headless) + the compositor. `RP_ERR_DEVICE` if no context.
  - `composite_driven(data, w, h, pitch, dupe, out_rgba, err)` — upload CPU RGBA to the core-frame texture
    (respecting `pitch`; `dupe` reuses the last texture), have `GLCompositor` draw it into the target
    framebuffer, then **headless:** `glReadPixels` the FBO into `out_rgba` (Y-flipped: GL is bottom-left
    origin, `out_rgba` is top-left); **windowed:** `SwapBuffers`.
  - `composite_and_present(...)` → `RP_ERR_UNSUPPORTED` (the presenting-frame path; unreachable for driven
    cores anyway — a presenting core whose `graphics_api` ≠ `RP_GFX_OPENGL` is rejected at `load_core`).
  - `allocate_surfaces(count, w, h, out, err)` → **`RP_OK` with `count` placeholder descs (zeroed handles),
    NOT `UNSUPPORTED`.** `Runtime::resize` on a loaded DRIVEN core calls `rebuild_surfaces → allocate_surfaces`
    (verified in `Runtime.cpp`), exactly as D3D11/Vulkan do; those descs are never consumed by a driven core
    (they carry no shared handle and driven cores don't read the surface ring), but the call must succeed or
    resizing a driven core under the GL host fails. GL creates no shared surfaces — the descs are inert.
  - `present_sync_handle` / `present_consume_sync_handle` / `present_device_uuid` → the `IRenderBackend`
    defaults (null / zeros).
- **`GLCompositor.{h,cpp}` + `GLShaders.h`** — one shader program (a textured fullscreen-quad: passthrough
  vertex + sample-the-core-texture fragment), a VAO/VBO, and the core-frame `GL_RGBA8` texture. `draw(tex)`
  renders the quad into the currently-bound framebuffer. GLSL lives inline in `GLShaders.h` (like the D3D11
  backend's `Shaders.h`).

Wiring:

- **`src/render/BackendFactory.cpp`**: `case RP_GFX_OPENGL: return std::make_unique<GLBackend>();`
- **`include/retropark/retropark_abi.h`**: add `RP_GFX_OPENGL = 3` to `rp_graphics_api`. **Additive enum
  value — NO `RETROPARK_ABI_VERSION` bump.** No struct changes; driven cores report `RP_GFX_NONE`; no core
  declares GL in A; `RP_GFX_OPENGL` is only ever passed to `rp_runtime_create`. The `load_core` api-match
  check (`presenting core must match runtime api`) is unaffected — driven cores skip it.
- **`harness/windowed/main.cpp`**: `--api gl` / `--api opengl` → `RP_GFX_OPENGL`.
- CMake: `src/render/gl/*.cpp` into the `retropark` lib; link `opengl32`, `gdi32` (system libs).

## Data flow (driven core under the GL host)

```
rp_runtime_create(RP_GFX_OPENGL)
  -> GLBackend.initialize  (WGL 3.3-core context; hidden window if headless, else the native HWND)
core run_frame -> video_refresh(CPU RGBA)
  -> Runtime.composite_driven
     -> GLBackend: upload RGBA -> GL_RGBA8 texture
     -> GLCompositor.draw(texture)  into the target framebuffer
     -> headless: glReadPixels(FBO) -> out_rgba (Y-flip)
        windowed: SwapBuffers
```

Identical to `composite_driven` on D3D11/Vulkan — the Runtime is unchanged; it already calls
`composite_driven` for driven cores regardless of backend.

## Error handling

- No GL 3.3 context (missing driver, GPU-less CI) → `initialize` returns `RP_ERR_DEVICE`. `probe_gl_shared()`
  lets tests skip cleanly (the Vulkan-tests pattern).
- Shader compile/link failure → `RP_ERR_DEVICE` with the info-log in `err`.
- `composite_and_present` (presenting-frame path) → `RP_ERR_UNSUPPORTED`; `allocate_surfaces` succeeds with
  inert descs so the driven-core resize path is unaffected (see the `GLBackend` note above).
- Never crash on a bad frame: a mismatched/oversize/`nullptr` frame is handled the way the existing backends
  handle it (dupe/skip), not asserted.

## Testing

- **`tests/test_gl_e2e.cpp`** (new), gated on `probe_gl_shared()` (skip if no capable context):
  - `RP_GFX_OPENGL` runtime → `resize(64,64)` → load `refcore_driven` → `present` → assert the green field +
    dynamic range readback (mirrors `test_driven_e2e` / `test_vulkan_e2e`).
  - Gated real-NES case (`RP_RUN_*` opt-in): FCEUmm software shim under the GL host → non-black + advancing
    (mirrors the existing driven NES e2e).
- **Windowed harness**: `--api gl` for visual confirmation on the real GPU.
- Registered in `run-headless-probes.sh` (findexe/probe-guarded) and the Windows CI job, with a graceful
  skip when no GL 3.3 device is available (same posture as the Vulkan probes — WARP/GPU-less runners skip).

## EverythingBox integration (user-selectable) + deploy

- **`native/src/emu/RetroParkRuntimeApi.h`**: `rpapi` gains `RP_GFX_OPENGL` as an available **driven** API.
  Driven cores currently resolve to `RP_GFX_D3D11`; the resolver takes the user's chosen driven backend.
  **Default stays D3D11** — the shipped NES path is not regressed; GL is opt-in.
- **User-facing setting** (per the two-settings-builders rule — add to BOTH the themed MainWindow settings
  surface AND the QWidget settings surface, or it is unreachable): **"RetroPark driven backend: D3D11
  (default) / OpenGL"**, global (a per-system override is not needed for A). Stored under the RetroPark
  settings namespace; read by the API resolver. All `#ifdef EB_HAVE_RETROPARK`.
- Presenting cores (gc → Dolphin) are unaffected — they always resolve to Vulkan regardless of the driven
  toggle.
- **Build**: EB's `retropark.lib` (ExternalProject) picks up `src/render/gl/`; links `opengl32`/`gdi32`
  (system). No new staged DLLs or cores (GL is a system DLL) — the deploy copies only the rebuilt exe.
- **Deploy**: bump EB's `external/RetroPark` submodule to the A commit, rebuild Release in a throwaway
  worktree off `origin/main` (per the shared-tree rule), targeted-copy `EverythingBox.exe` (+`.pdb`) to
  `C:\EverythingBox-app` (no `/MIR`), and live-verify a NES game rendering under the OpenGL backend via
  `EB_UITEST`.

## File-structure summary

| File | Responsibility |
|------|----------------|
| `src/render/gl/GLContext.{h,cpp}` | WGL 3.3-core context (headless/windowed), entrypoint loading, `probe_gl_shared` |
| `src/render/gl/GLBackend.{h,cpp}` | `IRenderBackend`: init, `composite_driven`; `composite_and_present`→UNSUPPORTED, `allocate_surfaces`→inert OK |
| `src/render/gl/GLCompositor.{h,cpp}` | textured fullscreen-quad draw of the core frame |
| `src/render/gl/GLShaders.h` | inline GLSL (vertex + fragment) |
| `src/render/BackendFactory.cpp` | `RP_GFX_OPENGL → GLBackend` |
| `include/retropark/retropark_abi.h` | `RP_GFX_OPENGL = 3` (additive, no version bump) |
| `harness/windowed/main.cpp` | `--api gl` |
| `tests/test_gl_e2e.cpp` | probe-gated green-field + NES e2e |
| EB `RetroParkRuntimeApi.h` + both settings builders | selectable "RetroPark driven backend: D3D11/OpenGL" |

## Open follow-ups (explicitly deferred)

- Subsystem B (HW-render libretro cores) — its own spec, builds on this.
- GLX/EGL context backends (Linux/Mac) behind the `GLContext` seam.
- GL presenting cores / GL↔Vulkan interop — not planned; each host backend serves its own API family.
