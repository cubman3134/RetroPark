# RetroPark — Slice C Design (driven execution model)

**Date:** 2026-08-06
**Status:** Approved (design), pending implementation plan
**Scope:** Build the second execution model — *driven* (libretro-style, host owns the frame
clock) — proving the pull-loop end to end with a reference driven core, on both GPU backends.

---

## 0. Context and goal

Slices A (D3D11) and B (Vulkan) built the **presenting** model: the core owns its render
loop/device/thread and pushes frames into a host-shared surface. That model is ideal for heavy
apps but gives up the host's ownership of the frame clock.

Slice C builds the **driven** model, which was declared in the ABI (`RP_CORE_DRIVEN`) but never
implemented (the runtime rejects driven cores with `RP_ERR_UNSUPPORTED`). In driven mode the
**host pulls**: it calls the core's `run_frame()` once per frame, the core renders into a **CPU
pixel buffer** and hands it back, and the host uploads it and runs it through the **existing
compositor + overlay**. Because the host owns the clock and the pixels, driven is the foundation
for the features that make retro emulation special — rewind, savestate, lockstep netplay — which
are *later* slices. This slice proves the execution path itself.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Framebuffer delivery | **Software only**: core renders a CPU RGBA8 buffer + `video_refresh` callback; host uploads to a sampled texture. **HW-render deferred.** |
| Timing | **One `run_frame` per `present`**, host-paced. Precise fps pacing (frame dup/drop, audio-sync) deferred. |
| Pixel format | **RGBA8 only** this slice. Format conversion (RGB565/XRGB8888) is a **libretro-shim** concern. |
| Audio / savestate | **Declared in the ABI, not implemented.** Audio and rewind/netplay are later slices. |
| Backends | Driven runs on **both** D3D11 and Vulkan — a driven core is **graphics-API-agnostic** (it emits CPU pixels; only the host backend differs). |
| Reuse | The compositor, overlay, input plumbing, and both backends are reused; driven adds a pull-loop + a CPU-upload op. |

### Why driven (recap)

Host-owns-clock + host-owns-pixels is exactly what makes rewind/savestate/deterministic-netplay
cheap: the host can snapshot and replay. Driven is the superpower for the retro library; presenting
is the superpower for heavy apps. Slice C makes the retro half real.

---

## 1. ABI evolution (additive; declares the future)

Driven cores use a different subset of `rp_core_abi` than presenting cores; the `type` flag
(`RP_CORE_DRIVEN` vs `RP_CORE_PRESENTING`) tells the host which path to drive. All additions are
**additive** — presenting cores leave the new fields null and the entire A+B suite stays green.

### 1.1 New types + core entry points

```c
typedef struct rp_av_info {
    double   fps;            /* target frames/sec (informational this slice) */
    double   sample_rate;    /* audio Hz; 0 if silent (audio deferred) */
    uint32_t base_width;     /* nominal framebuffer size */
    uint32_t base_height;
    uint32_t max_width;      /* max the core may output */
    uint32_t max_height;
    uint32_t pixel_format;   /* rp_pixel_format; RGBA8 only this slice */
} rp_av_info;
```

Added to `rp_core_abi` (presenting cores set these to NULL):

```c
void      (*get_av_info)(rp_core* core, rp_av_info* out);
void      (*run_frame)(rp_core* core);       /* advance exactly one frame */
/* Declared-but-stubbed for the rewind/netplay slice: */
size_t    (*serialize_size)(rp_core* core);
rp_result (*serialize)(rp_core* core, void* data, size_t size);
rp_result (*unserialize)(rp_core* core, const void* data, size_t size);
```

### 1.2 New host callbacks

Added to `rp_host_iface` (the core calls these during `run_frame`):

```c
/* Present a finished CPU framebuffer. data==NULL means "duplicate the last frame". */
void (*video_refresh)(rp_host* host, const void* data, uint32_t width,
                      uint32_t height, uint32_t pitch);
/* Declared-but-stubbed (audio deferred): */
void (*audio_sample)(rp_host* host, const int16_t* frames, size_t num_frames);
```

### 1.3 API-agnostic driven cores

A driven core emits CPU pixels and touches no GPU API, so the presenting-only "core
`graphics_api` must equal the host backend" rule does **not** apply to it. Add
`RP_GFX_NONE` to `rp_graphics_api`; driven manifests declare `"graphics_api": "none"`. The
runtime enforces the api-match **only for presenting cores**; a driven core loads on any backend.

The `rp_surface_set`/`submit_frame`/`sync_value` machinery from Slice B is untouched (presenting
only). `RETROPARK_ABI_VERSION` bumps to `3` (the `rp_core_abi`/`rp_host_iface` shapes grew).

---

## 2. Architecture (driven path, parallel to presenting)

Driven is single-threaded and host-paced — no core thread, no shared GPU surface, no timeline
semaphore. The core renders into a CPU buffer; the backend uploads it into an internal **sampled
texture** and the *existing* compositor draws it + the blended overlay.

| Presenting (A/B) | Driven (C) |
|---|---|
| Core owns render thread + GPU device | No core thread; host calls `run_frame` |
| Shared GPU surface + sync (keyed mutex / timeline) | CPU buffer → host uploads to a sampled texture |
| `set_surfaces`/`start`/`stop` | `get_av_info`/`run_frame` |
| API-specific (surface sharing) | **API-agnostic** (CPU pixels) |
| `composite_and_present(ready_index, sync_value, …)` | `composite_driven(data, w, h, pitch, dupe, …)` |

**New `IRenderBackend` op (both D3D11 + Vulkan implement it):**

```cpp
// Upload a CPU RGBA8 framebuffer (respecting pitch) into an internal sampled texture and
// composite it + the overlay; present, or read back into out_rgba (headless).
// dupe==true reuses the last uploaded texture (data ignored).
virtual rp_result composite_driven(const void* data, uint32_t width, uint32_t height,
                                    uint32_t pitch, bool dupe, uint8_t* out_rgba,
                                    std::string& err) = 0;
```

It reuses each backend's existing compositor render (fullscreen sample of the core texture +
blended overlay) — only the *source* of the core texture differs (a CPU upload instead of a shared
surface). No QFOT, no cross-device sync — a plain `vkCmdCopyBufferToImage` / `UpdateSubresource`.

## 3. Data flow (driven, one frame)

1. **Load:** runtime reads the manifest (`type: driven`, `graphics_api: none`), loads the core,
   skips the presenting api-match, calls `get_av_info` → records geometry/fps.
2. **Drive:** on `present()`, the runtime calls the core's `run_frame()`.
3. **Frame handoff:** inside `run_frame`, the core fills its CPU buffer and calls
   `host->video_refresh(data, w, h, pitch)`; the runtime's trampoline records `(data, w, h, pitch)`
   (and a `dupe` flag when `data==NULL`).
4. **Composite:** after `run_frame` returns (the core's buffer is still valid), the runtime calls
   `backend->composite_driven(data, w, h, pitch, dupe, out_rgba, err)` — upload → compositor →
   present/readback. The pointer's lifetime is exactly this sequential window.
5. **Input:** the host feeds `input_state` as today; the driven core reads it each `run_frame`.

The runtime dispatches by `core_type_`: presenting cores keep the Slice-A/B `composite_and_present`
path; driven cores take the `run_frame` + `composite_driven` path. Everything downstream of "a core
texture exists" (compositor, overlay, present, readback) is shared.

## 4. Error handling

- **Load-time:** a `driven` core missing `run_frame`/`get_av_info`, or `get_av_info` returning zero
  geometry → refuse with a specific error (never a partial load). A driven core declaring a
  `pixel_format` other than RGBA8 → `RP_ERR_UNSUPPORTED` (conversion is the shim's job).
- **Frame-time:** `video_refresh(data==NULL)` → **duplicate the last frame** (libretro convention):
  keep the previous texture, overlay stays live, no crash. `width/height` beyond `max_*`, or a
  `pitch < width*4` → skip the frame rather than upload garbage.
- **Pitch/stride:** `pitch` (bytes/row) may exceed `width*4` (padding). The upload copies `width*4`
  per row from a pitch-strided source; mishandling stripes the image, so it is explicitly tested.
- **Backend upload failure** → `RP_ERR_DEVICE`, clean. Reuses the runtime's existing null-backend /
  lifecycle guards.
- **Crash honesty:** a bad `run_frame` can still fault the host; out-of-process isolation stays a
  later slice and is not claimed here.

## 5. Testing

- **Pure logic:** `rp_av_info` validation; the driven-vs-presenting dispatch; the **pitch-respecting
  row-copy** math (unit-tested with a padded pitch, no GPU).
- **Backend upload (headless, both backends):** upload a known CPU pattern *with non-trivial pitch
  padding* → composite → read back → assert the pattern lands correctly (proves pitch handling) AND
  the overlay blends. D3D11 via WARP; Vulkan probe-guarded on the real GPU. The driven analog of the
  handoff tests, but far simpler (a plain upload, no cross-device sync).
- **Reference driven core:** a small DLL whose `run_frame` fills a CPU buffer with the same
  green-rising-blue pattern the presenting cores use (so e2e assertions mirror exactly). It touches
  no GPU API — the *same* core binary runs under both backends.
- **Driven e2e (both backends):** load the driven core via the C API, drive frames, assert green core
  + blended overlay; plus reload + not-bricked. A `video_refresh(NULL)` dupe case asserts the last
  frame persists.
- **Regression:** the entire A+B presenting suite stays green — the ABI change is additive and the
  presenting path is untouched (including the ABI-version bump consumers).

## 6. Scope line

**In Slice C:**
- ABI: `rp_av_info`, `get_av_info`, `run_frame`, `video_refresh`; `RP_GFX_NONE`; declared-stub
  `serialize`/`serialize_size`/`unserialize` + `audio_sample`; `RETROPARK_ABI_VERSION` → 3.
- Runtime driven dispatch by core type (load → get_av_info → `run_frame` per present →
  `composite_driven`); api-match enforced for presenting cores only.
- `IRenderBackend::composite_driven` (CPU upload + composite) on **both** D3D11 and Vulkan.
- A reference driven core (RGBA8, animated, API-agnostic).
- Driven upload + e2e tests on both backends + reload/not-bricked + the dupe case.
- The windowed harness able to run the driven core.

**Explicitly out (deferred):**
- **Audio** (declared, not implemented).
- **Savestate/serialize** (declared, not implemented) → rewind, netplay.
- HW-render driven cores; precise fps pacing (frame dup/drop, audio-sync).
- Pixel-format conversion (RGB565/XRGB8888) → the **libretro shim** (its own later slice).
- Everything already deferred: cross-API interop, wrapping real heavy apps, out-of-process
  isolation, real network download/catalog, iOS/Android, EverythingBox integration.

**The single provable claim of Slice C:** *a driven core, pulled by the host one `run_frame` per
`present`, hands back a CPU framebuffer that the host uploads and composites with a blended overlay —
on both D3D11 and Vulkan — with the entire presenting (A+B) suite still green.*

## 7. Repo additions

```
RetroPark/
  include/retropark/retropark_abi.h        # + rp_av_info, get_av_info/run_frame/serialize*,
                                           #   video_refresh/audio_sample, RP_GFX_NONE, ABI v3
  src/render/
    IRenderBackend.h                        # + composite_driven (pure virtual)
    d3d11/D3D11Backend.h/.cpp               # composite_driven (UpdateSubresource upload)
    vulkan/VulkanBackend.h/.cpp             # composite_driven (staging buffer → copy → sample)
  src/runtime/Runtime.h/.cpp                # driven dispatch, video_refresh trampoline, core_type_
  src/loader/Manifest.cpp                   # accept "none" graphics_api
  cores/refcore_driven/
    RefCoreDriven.cpp                        # CPU RGBA8 animated framebuffer, run_frame
    core.json                                # type: driven, graphics_api: none
    CMakeLists.txt
  tests/
    test_driven_upload.cpp                   # backend upload + pitch, both backends
    test_driven_e2e.cpp                      # driven core via C API, both backends, dupe case
```
