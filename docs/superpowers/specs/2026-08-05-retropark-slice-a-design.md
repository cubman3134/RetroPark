# RetroPark — Slice A Design

**Date:** 2026-08-05
**Status:** Approved (design), pending implementation plan
**Scope:** First provable slice of RetroPark — the core ABI + a desktop runtime that
loads a core at runtime and composites an overlay over it, on Windows.

---

## 0. What RetroPark is (context)

RetroPark is to EverythingBox what libretro is to RetroArch: a runtime that lets a
frontend load runnable modules ("cores") on the fly and drive them through a stable
contract. The long-term goals that separate it from libretro:

- **Heavy apps as first-class citizens.** Dolphin, RPCS3, and media apps like VLC own
  their own render loops, threads, and GPU devices. libretro's single frame-driven model
  makes them slow or broken. RetroPark must run them at full speed, in the same screen,
  with overlays.
- **Cross-platform, runtime-loaded cores** — Windows/Android/desktop can load native code
  at runtime; iOS cannot (Apple forbids downloading/executing native code — this is why
  RetroArch statically links its cores on iOS). iOS therefore needs a fundamentally
  different strategy (bundled cores / interpreted plugins / remote execution) and is
  **out of scope for every early slice**; it is called out here only so the architecture
  does not assume iOS works like the others.
- **A libretro compatibility shim** eventually, so existing libretro cores load unmodified.

This document specifies **only Slice A**. Each later capability is its own spec → plan →
build cycle against the interfaces Slice A establishes.

### Decisions already made (the spine)

| Decision | Choice |
|---|---|
| First slice | Core ABI + desktop runtime: load one core at runtime, composite an overlay, own window, Windows |
| Host runtime language | C++, built as a **standalone library with its own C API** — no Qt, no EverythingBox coupling |
| Core boundary | **Flat C ABI** (stable across compilers and source languages, like libretro) |
| Execution models | **Two**: *presenting* (core owns its loop, renders into a host surface) and *driven* (libretro-style, host owns the loop). **Presenting is proven in Slice A; driven is declared in the ABI but not implemented.** |
| Graphics | **Thin render abstraction** (`IRenderBackend`); the abstraction is the **surface + compositing boundary** (cores render with their own native API into a host-provided shared surface), with an optional draw-helper for simple cores as a *later* convenience |
| First backend | **D3D11** (fastest to a working, testable result on Windows); **Vulkan is backend #2 in a later slice**, which is what proves the abstraction is not secretly D3D-shaped |

### Why two execution models (the core thesis)

- **Driven** (libretro-style): host calls `run_frame()`, core returns one framebuffer +
  audio. The host owning the frame clock and memory is *why* driven cores get automatic
  rewind, savestate-scrubbing, and deterministic lockstep netplay. It is ideal for retro
  cores and a **bottleneck for heavy apps**, which get their speed from CPU/GPU/audio
  threads that must *not* wait on each other.
- **Presenting**: the core keeps its own loop, threads, and GPU device and renders into a
  host-provided shared surface; the host still owns the **final composite** (overlays) and
  input routing, but not the frame clock. Zero forced lockstep, ~zero extra copy — full
  speed for heavy apps. The cost is that the host no longer owns the clock/memory, so
  rewind/savestate/netplay become the core's responsibility (heavy emulators have their
  own anyway).

Supporting both, with the core declaring its type, is the design: driven where it is a
superpower (retro library), presenting where libretro fails (the heavy hitters). Slice A
proves the **hard** one first to de-risk the whole premise.

---

## 1. Architecture

Three parts, none referencing Qt or EverythingBox:

1. **Runtime host** — the `retropark` library. Loads cores, owns the window's final
   surface, composites, routes input. Exposes a C API to frontends.
2. **Reference presenting core** — `refcore_present.dll`. A self-contained module that
   **owns its own D3D11 device and its own render thread** and draws an animated scene into
   a surface the host hands it. Deliberately built to model a *heavy app*: separate GPU
   device, separate loop, never driven frame-by-frame.
3. **Test harness** — a minimal Win32 windowed app that spins up the runtime and shows the
   result, plus a headless variant for automated pixel tests.

**The critical realism:** core and host use **two separate D3D11 devices**, exactly as a
real heavy emulator would. They share pixels via a **shared texture (shared handle +
`IDXGIKeyedMutex`)** — the standard Windows primitive for handing a texture between devices
and threads. If a core with its own device and loop can render into our surface while we
composite an overlay over it, wrapping a real heavy app later is the *same mechanism*.

```
 frontend (test harness / later: EverythingBox)
        │  retropark.h  (C API)
        ▼
 ┌─────────────────────── retropark runtime ───────────────────────┐
 │  Loader ── Compositor ── IRenderBackend → D3D11Backend           │
 │     │                         ▲  (host device, shared-tex ring)  │
 │     │ LoadLibrary / ABI       │                                  │
 └─────┼─────────────────────────┼──────────────────────────────────┘
       │ retropark_abi.h         │ shared texture (handle + keyed mutex)
       ▼                         │
 ┌── refcore_present.dll ────────┼──┐
 │  own D3D11 device + own render thread │  → submit_frame(index)
 └───────────────────────────────────┘
```

## 2. Components

| Component | Responsibility | Interface |
|---|---|---|
| `retropark.h` | Public C API for frontends: create/destroy runtime, load/unload core, pump, resize, feed input | C API |
| `retropark_abi.h` | The flat C ABI a core exports: abi version, core type, lifecycle, present hooks; **audio + driven declared but not implemented in Slice A** | C ABI |
| **Loader** | Discover core package → `LoadLibrary` → resolve export table → ABI-version check → lifecycle (load/create/start/stop/destroy/unload) | internal |
| **Render backend** | `IRenderBackend` interface + **`D3D11Backend`**: allocate shared surfaces, open cross-device handles, present host swapchain | C++ interface |
| **Compositor** | One GPU pass: draw core surface fullscreen, then overlay **with blend/tint** (proves libretro-quality compositing, not a floating layer) | internal |
| `refcore_present.dll` | Reference presenting core: own device + own thread, renders an animated scene into the shared surface | exports the ABI |

### Core package (on disk)

A core is a folder:

```
cores/refcore_present/
  core.json        # manifest
  refcore_present.dll
  <assets…>        # none in Slice A
```

`core.json`:

```json
{
  "id": "refcore_present",
  "name": "Reference Presenting Core",
  "type": "presenting",         // presenting | driven  (driven not implemented in Slice A)
  "abi_version": 1,
  "graphics_api": "d3d11",      // the API the core renders with
  "entry": "refcore_present.dll"
}
```

On desktop, "download and run at runtime" is simply: fetch that folder into the cores
directory and `LoadLibrary` the entry — no restart, because dynamic loading is native
here. (Network download/catalog is a later slice; Slice A loads from a local folder and
treats "download" as a fetch-to-folder stub.)

## 3. Data flow — presenting core, one frame

1. **Load:** host reads `core.json` → `LoadLibrary` → resolves export table → checks
   `abi_version`.
2. **Create:** host calls the core's `create(host_iface*)`, handing it a callback table
   (log, input_state, submit_frame). Core creates **its own** D3D11 device and starts
   **its own** render thread.
3. **Surface handoff:** host allocates a **ring of 2–3 shared textures** sized to the
   window and passes their shared handles to the core, which opens them on its device.
4. **Core renders (its cadence):** acquire keyed mutex on the current surface → draw →
   release mutex → `submit_frame(index)`. The core is **never stalled waiting on the
   host** — that decoupling is what keeps heavy apps fast.
5. **Host composites (its cadence):** acquire that surface's mutex → draw core texture
   fullscreen → draw overlay quad + text **blended over it** → present host swapchain.
6. **Input:** host polls keyboard/gamepad and exposes state via
   `host_iface->input_state()`; core reads it inside its own loop.
7. **Resize / teardown:** on resize the host reallocates the ring and re-hands it; on
   teardown the host calls the core's `stop`/`destroy`, the core joins its thread and
   releases its device, and the host `FreeLibrary`s.

The **ring of surfaces + keyed mutex** is the heart of the design: it lets the core and
host run on independent clocks (the anti-lockstep property that makes heavy apps fast)
while still giving the host the final composite for overlays.

## 4. Error handling

Cores are **untrusted native code across a C ABI**; the host defends the boundary:

- **Load-time:** ABI-version mismatch, missing exports, or malformed manifest → refuse to
  load with a specific error; never a partial load.
- **Create-time:** core device/thread creation failure → host tears down cleanly and
  reports it.
- **Frame-time liveness:** if a core never submits (hung thread), the host keeps
  compositing the **last-good surface** (overlay stays live) rather than freezing;
  keyed-mutex acquires use **timeouts**, never infinite waits, so a misbehaving core
  cannot deadlock the compositor.
- **Resize races:** each surface ring carries a **generation counter**; frames submitted
  against a stale ring are dropped, never drawn into a freed texture.
- **Crash honesty:** a bad core *can* still fault the host process. True isolation means
  **out-of-process cores**, which is a deliberate later slice. Slice A does the affordable
  version — SEH guards (`__try/__except`) around calls *into* the core on Windows to
  convert some faults into a logged, recoverable "core faulted" state rather than silent
  process death. **This is not sandboxing** and is called out as such.

## 5. Testing

Split by testability:

- **Real TDD (pure logic):** manifest parsing, ABI-version negotiation, the loader
  lifecycle state machine, and the surface-ring / generation bookkeeping — covered
  test-first.
- **FFI / lifecycle:** a tiny **mock core** `.dll` exporting the ABI, driven through
  `load → create → handoff → submit → destroy` to assert the loader contract without real
  GPU work.
- **Compositor (headless):** render N frames offscreen, read back pixels, and assert two
  things — the core's known test pattern appears where expected, **and** an overlay pixel
  is present *and blended* (the tint provably altered the underlying color). That second
  assertion proves we are **compositing**, not floating a layer — the whole differentiator.
- **On-screen smoke:** the windowed harness for human verification.

Build system is **CMake** (matches EverythingBox and lets the Vulkan/Metal backends and
other platforms drop in later).

## 6. Scope line

**In Slice A:**

- Public C API (`retropark.h`) + core ABI (`retropark_abi.h`) headers — both core types
  *declared*, driven reserved.
- Loader + manifest + lifecycle.
- `IRenderBackend` + **D3D11 backend**.
- Shared-texture ring + keyed-mutex handoff.
- Compositor with **blended/tinted overlay + text**.
- `refcore_present.dll` (own device + own thread).
- Windowed + headless test harness and the tests above.

**Explicitly out (named so they cannot creep in):**

- Vulkan backend (next slice — it is what proves the abstraction).
- The **driven** execution path's implementation (type reserved, not built).
- libretro compatibility shim.
- Wrapping any real heavy app (Dolphin / RPCS3 / VLC).
- Out-of-process crash isolation / sandboxing.
- Real network download / catalog (local folder load only; "download" is a fetch-to-folder
  stub).
- **Audio** (declared in the ABI; the reference core is silent).
- iOS / Android.
- EverythingBox integration.

**The single provable claim of Slice A:** *a core with its own GPU device and its own
render loop renders into our surface, and we composite a real blended overlay on top, at
each side's own cadence — on Windows via D3D11.* Everything else is a later slice against
the interfaces this one establishes.

## 7. Proposed repository layout

```
RetroPark/
  CMakeLists.txt
  include/retropark/
    retropark.h              # public C API (frontend-facing)
    retropark_abi.h          # the core ABI (core-facing)
  src/
    loader/                  # package discovery, LoadLibrary, lifecycle
    render/
      IRenderBackend.h
      d3d11/                 # D3D11Backend, shared-texture ring, compositor
    runtime/                 # ties loader + backend + input into the C API
  cores/
    refcore_present/         # reference presenting core (own device + thread)
  harness/
    windowed/                # Win32 smoke app
    headless/                # offscreen render + pixel assertions
  tests/                     # unit + FFI/lifecycle tests, mock core
  docs/superpowers/specs/    # this document
```
