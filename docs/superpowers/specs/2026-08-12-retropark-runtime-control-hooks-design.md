# RetroPark — Runtime Control Hooks (libretro-style hotkeys/menus) Design

**Date:** 2026-08-12
**Status:** Approved (design)
**Scope:** Add the small runtime-side control surface a frontend needs to build a RetroArch-style experience
(hotkeys, an in-game menu, save/load slots, pause, reset, OSD) **on top of** RetroPark. The runtime stays the
"dumb engine" (the libretro split); the frontend (EverythingBox) owns all UI. **Phase 1: no `rp_core_abi`
bump** — everything reuses existing primitives or is runtime-only. True presenting-core pause/fast-forward is
an explicit **Phase 2** follow-up (ABI bump + per-core work) and is OUT of this spec.

---

## 0. Context

RetroPark is the runtime; EverythingBox is the frontend that embeds it. RetroPark already exposes the
primitives a frontend needs for most of this: `rp_runtime_save_state` / `load_state` / `serialize_size`,
`rewind` / `set_rewind`, `set_input`, and `present` (which reads the composited frame back into a caller
buffer). The Runtime knows each core's `rp_core_type` (presenting vs driven) from `get_info`, and
`VulkanBackend` already composites the core image (+ an overlay) into an offscreen/swap target. What's missing
is a handful of *control* hooks and a status query so a frontend menu can pause, reset, and reflect state.

**Chosen model (user):** RetroPark exposes hooks; the frontend does the UI ("like libretro"). **Chosen scope
(user):** Phase 1 = the no-core-change actions + a pragmatic "soft pause" for presenting cores; defer true
presenting-core pause/fast-forward to a Phase 2 ABI bump.

## 1. The control surface (added to `include/retropark/retropark.h`)

```c
rp_result rp_runtime_pause (rp_runtime* rt);   // driven: stop advancing; presenting: freeze display + mute
rp_result rp_runtime_resume(rp_runtime* rt);
rp_result rp_runtime_reset (rp_runtime* rt);   // stop + reboot the current content (Phase 1: full reboot)

typedef struct rp_runtime_status {
    uint32_t core_type;       // rp_core_type: RP_CORE_PRESENTING / RP_CORE_DRIVEN
    uint32_t graphics_api;    // rp_graphics_api
    int32_t  paused;          // 0/1
    int32_t  content_loaded;  // 0/1
    double   fps;             // measured present rate (menu / OSD)
} rp_runtime_status;
rp_result rp_runtime_get_status(rp_runtime* rt, rp_runtime_status* out);
```

Four functions + one struct. That is the entire new API. Notes:
- **Screenshot needs nothing new:** the frontend captures by calling `rp_runtime_present(rt, buf)` (already a
  composited-frame readback) and saving `buf`.
- **Save slots need nothing new:** the frontend calls `save_state`/`load_state` against different files/buffers.
- **Hotkeys, menu, OSD, rebinding, per-core config:** 100% frontend. No runtime surface.
- Idempotency: `pause` while paused / `resume` while running return `RP_OK` (no-ops). `get_status`,
  `pause/resume/reset` with no content loaded return `RP_OK` with `content_loaded=0` (pause/resume/reset become
  no-ops); `get_status(out==null)` → `RP_ERR_BAD_ARG`.

## 2. Pause semantics — the driven/presenting split

The `paused` flag lives on the `Runtime`. `rp_runtime_pause`/`resume` set it and branch on the loaded core's
`rp_core_type`:

- **Driven cores** (`refcore_driven`, `libretro_shim`): when `paused`, `rp_runtime_advance` is a no-op and
  `rp_runtime_present`/`render` re-composite the last core image. A real freeze; frontend timing is irrelevant.
- **Presenting cores** (RPCS3, Dolphin, `refcore_present_vk`): **soft pause** —
  1. **Display freeze:** the compositor stops acquiring a *new* ring slot and re-composites the last-acquired
     shared image. Concretely, `VulkanBackend::composite_and_present` treats `paused` as "no new frame" (use
     the previously acquired slot; skip the QFOT-acquire + timeline advance for a new `sync_value`). The
     already-present stale-frame guard (`sync_value > last_present_sync_`) makes this natural.
  2. **Audio mute:** the Runtime's audio-forward path (host `audio_sample` sink) drops/zeros samples while
     `paused` so the frozen game is silent.
  The emulator keeps simulating underneath. For RPCS3 specifically, the timeline lock-step means "host stops
  consuming" back-pressures the core's render thread, so the GPU side stalls close to genuinely.

This is the only real runtime work; everything else is plumbing/branching.

## 3. Reset

Phase 1 `rp_runtime_reset` = **stop + reboot the current content** through the existing lifecycle: keep the
core module loaded, tear down the running instance (stop/destroy or the core's stop), and re-run
`load_content` with the same path (re-`set_surfaces` + `start`). Slower than a native `retro_reset`, but needs
no core hook. (Phase 2's ABI adds a fast per-core `reset`.) `reset` clears `paused`.

## 4. Frontend boundary (EverythingBox — informational, not built here)

The menu-open dance the frontend owns: on open → `rp_runtime_pause`, stop feeding game input, draw the menu
over the frame from `present`; poll `rp_runtime_get_status` for fps/paused; on close → `rp_runtime_resume`.
Hotkeys map to `pause/resume`, `save_state`/`load_state` (+ slot files), `rewind`, `reset`, screenshot
(`present`→save). None of this lives in RetroPark.

## 5. Testing

Headless doctests in the existing `retropark_tests` suite (no real emulator needed):
- **Driven** (`refcore_driven`): `pause` → `advance` no-op (frame counter frozen) → `resume` → ticks again.
- **Presenting** (`refcore_present_vk`, which animates a rising-blue clear): `pause` → consecutive `present`
  readbacks are **byte-identical** (display frozen) + audio-forward is silent → `resume` → frames change again.
- **Reset** (`refcore_present_vk` / `refcore_driven`): content reboots — the ref core's frame counter /
  generation resets.
- **`get_status`**: correct `core_type`, `paused` transitions, `content_loaded`, and a plausible `fps`.
- **Arg-guard**: `get_status(null)` → `RP_ERR_BAD_ARG`; control calls with no content → `RP_OK`, no crash.
- **Regression**: `dolphin_present` / `refcore_present_vk` normal load/run path unchanged.
- **Harness smoke test**: wire a Pause key + a Reset key into the windowed harness to eyeball it live.

## 6. Scope

**In:** the four control functions + `rp_runtime_status`; the runtime `paused` flag; the compositor
display-freeze + audio-mute for presenting cores; the driven no-op-advance freeze; reboot-based `reset`; the
doctests + a harness smoke test. **No `rp_core_abi` bump.**

**Out (Phase 2, its own spec):** true presenting-core pause/resume/`set_speed`/fast `reset` via an
`rp_core_abi` bump implemented in RPCS3 (`Emu.Pause/Resume`) and Dolphin (`Core::SetState`); fast-forward for
presenting cores; input remapping, cheats, shader UI, and all menu UI (frontend).

**The single provable claim:** *RetroPark exposes `pause`/`resume`/`reset`/`get_status`; a frontend can pause a
running core (driven = true freeze, presenting = frozen display + muted audio), reset it, and query its state —
enough to build a RetroArch-style hotkey + menu layer entirely in the frontend, with no core-ABI change.*

## 7. Repo additions

```
include/retropark/retropark.h                 # +4 functions, +rp_runtime_status (committed)
src/runtime/Runtime.{h,cpp}                    # paused flag, pause/resume/reset/get_status, fps measure
src/render/vulkan/VulkanBackend.{h,cpp}        # composite honors "paused" (freeze last frame)
tests/test_runtime_control.cpp                 # the doctests above
harness/windowed/main.cpp                      # Pause + Reset hotkeys (smoke test)
```
