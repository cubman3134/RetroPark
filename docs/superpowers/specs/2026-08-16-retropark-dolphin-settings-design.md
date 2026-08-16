# Dolphin/GameCube graphics settings via the core-options channel — Design

**Date:** 2026-08-16
**Status:** Approved (pre-plan)

## Goal

Expose the in-process RetroPark Dolphin's meaningful graphics knobs — **internal
resolution** and **aspect ratio** — to the user, and fix the bug where GameCube
gfx settings are written to a standalone `GFX.ini` the in-process Dolphin never
reads. Reuse the v9 core-options channel already built for driven libretro-shim
cores, so GameCube gets the same in-game + global settings UI with **no new
ABI** and no parallel settings path.

Out of scope (v1): MSAA and other Dolphin knobs (internal-res + aspect are the
two confirmed meaningful for the shared-surface handoff); RPCS3; the standalone
(non-RetroPark) Dolphin path (unchanged).

## Why this shape

- The v9 `rp_core_abi` already carries `core_options_json` / `core_option_get` /
  `core_option_set`; the Dolphin presenting vehicle currently leaves them NULL.
  Implementing them for Dolphin lights up the exact EB surfaces already shipped
  for NES (in-game pause "Core Options", global editor, per-game overrides,
  persistence) — zero new ABI, zero new `rp_runtime_*` call.
- Only **internal resolution** and **aspect ratio** matter for RetroPark-GC:
  the renderer is locked to Vulkan (the shared-surface handoff requires it) and
  vsync is host-owned (the RetroPark compositor owns presentation). So the
  standalone gfx "quartet" has two knobs that are inert for RetroPark-GC — the
  core-options list exposes only the two that apply.
- **Performance:** both knobs are applied via `Config::SetBaseOrCurrent` once at
  boot (the persisted value) — zero per-frame cost. Live changes re-apply only
  on a dirty flag (never per-frame). The one framerate lever is the
  internal-resolution *value* itself (supersampling cost), which is the user's
  choice. The channel adds no steady-state overhead.

## Architecture & data flow

One settings channel for all cores. GameCube behaves like NES from the UI's
perspective.

```
Dolphin vehicle (rp_dolphin.cpp): core_options_json -> [internal-res, aspect]
                                  core_option_set(key,val) -> Config::SetBaseOrCurrent + live refresh
runtime (unchanged): rp_runtime_core_options_json/_get/_set already forward to the loaded core
EB RetroParkView (PRESENTING branch, extended): harvest running Dolphin options ->
    cache descriptors -> apply persisted effective values post-boot; in-game "Core Options" page
EB global editor: GC system's RetroPark arm harvests dolphin options (headless-capable)
EB editLaunchOptions: HIDE the gfx-quartet rows for RetroPark-GC (kills the misdirect)
persistence: Settings opt/<gc-core>/* + optgame/<token>/<gc-core>/* (same keying as driven)
```

Unlike fceumm, Dolphin declares its (static) option set immediately at core
create — so the options are harvestable **headlessly** (the global editor works
for GC pre-launch, no "launch once" fallback needed).

## Component 1 — Dolphin vehicle (`external/dolphin/.../rp_dolphin.cpp`)

The vehicle already sets Dolphin config at boot via `Config::SetBaseOrCurrent`
(backend=Vulkan, EFB hacks, audio, SI devices). Add the core-options exports:

- **`core_options_json`** returns a fixed descriptor (JSON array, the v9 shape
  `[{key,desc,info,default,values:[{value,label}]}]`):
  - `dolphin_internal_resolution` — desc "Internal Resolution"; values
    `1`→"Native (640×480)", `2`→"2× (1280×960)", `3`→"3×", `4`→"4×", `5`→"5×",
    `6`→"6×"; default `1`.
  - `dolphin_aspect_ratio` — desc "Aspect Ratio"; values `0`→"Auto", `1`→"Force
    16:9", `2`→"Force 4:3", `3`→"Stretch"; default `0`.
  (`GFX_EFB_SCALE` is a plain int: `1`=native, `N`=N×. For aspect, **verify the
  actual `AspectMode` ordinal values in THIS Dolphin tree's
  `VideoCommon/VideoConfig.h` before wiring** — the enum has been reordered across
  Dolphin versions, so do NOT assume `Auto=0, AnalogWide=1, Analog=2, Stretch=3`;
  map the option value strings to whatever the enum actually is in-tree.)
- **`core_option_set(key, value)`** stores the override and applies it:
  - `dolphin_internal_resolution` → `Config::SetBaseOrCurrent(Config::GFX_EFB_SCALE, atoi(value))`.
  - `dolphin_aspect_ratio` → `Config::SetBaseOrCurrent(Config::GFX_ASPECT_RATIO, (AspectMode)atoi(value))`.
  - Apply is valid both **before boot** (stashed values applied in the existing
    boot config block, so first frame is correct) and **live** while running
    (Dolphin re-derives `VideoConfig` on a config change — internal-res triggers
    a render-target recreation, a one-time hitch, no steady-state cost). Guard
    live application to the CPU/video thread as Dolphin requires.
- **`core_option_get`** returns the current override-else-default value string.
- Wire the three into the vehicle's `kAbi` (replacing the NULL slots). No
  ABI-version change (v9 already has the slots). The vehicle DLL is rebuilt
  (`rp_dolphin.cpp` changed) but stays ABI v9.

**Threading note:** the vehicle runs Dolphin on its own host thread; option sets
arriving from EB's UI thread must hand the `Config::Set` + refresh to Dolphin's
CPU thread (the vehicle already has the `RunOnCPUThread`/promise pattern from the
savestate slice) to avoid a data race on a live change. A pre-boot set just
stashes into a map the boot block reads.

## Component 2 — EverythingBox integration

- **In-game "Core Options" (primary surface):** extend the RetroParkView
  harvest + in-game editor (built for the driven branch) to the **presenting**
  branch. The pause-menu "Core Options" button already gates on
  `rp_runtime_core_options_json(rt_) != "[]"`, so it appears automatically once
  the vehicle returns options; the live-apply + scoped-persist path is shared.
- **Launch-apply:** extend the post-content harvest + apply block to the
  presenting branch (today it is inside the driven branch only). After the
  presenting `load_content`, harvest the running Dolphin options, cache
  descriptors, and push persisted effective values via `rp_runtime_core_option_set`.
- **Global editor:** extend the RetroPark arm to the GC system. Because GC's
  `externalEmulator` is "dolphin" it is currently excluded from the Options…
  action — for the RetroPark-GC case, present the core-options editor instead
  (Dolphin options are headless-harvestable, so no "launch once" row needed).
- **Hide the gfx-quartet for RetroPark-GC:** in `editLaunchOptions`, when a GC
  game resolves to RetroPark-presenting (the `retroParkStandaloneDivert` /
  effective-backend decision), do NOT render the standalone gfx-quartet rows
  (they write a `GFX.ini` the in-process Dolphin ignores). Standalone-Dolphin GC
  keeps the quartet unchanged.
- **Persistence key:** the GC system's core name via `Settings::coreFor(systemId)`
  fallback `cores[0]` — same mechanism as the driven cores; for GC this resolves
  to the Dolphin/GC core id. RetroPark-GC options live under `opt/<that>/*`.

## Error handling

- A non-GC / driven core: the vehicle change doesn't affect it; driven cores keep
  their own options.
- An unknown key to `core_option_set`: `RP_ERR_NOT_FOUND` (matches the shim).
- A live set that arrives before boot completes: stashed and applied at boot.
- Renderer/vsync are never exposed for RetroPark-GC, so no inert knobs.

## Testing

- **RetroPark (gated, like the Dolphin e2e):** load `dolphin_present` headless,
  assert `core_options_json` contains `dolphin_internal_resolution` +
  `dolphin_aspect_ratio` with the expected values; `core_option_set` then
  `core_option_get` echoes; boot a GC ISO and assert a set internal-resolution is
  reflected in Dolphin's `VideoConfig` (`iEFBScale`) — gated behind the existing
  Dolphin-e2e env gate + ISO.
- **EverythingBox:** the presenting-branch apply path compiles + the in-game page
  appears for a GC game (deferred in-app UITEST like the driven side); a headless
  check that a persisted GC option round-trips through `Settings`.

## Scope & done-bar

In scope: internal-res + aspect for RetroPark-GC via the core-options channel;
the three EB surfaces extended to presenting; quartet hidden for RetroPark-GC.
**Done-bar:** EB-integrated **and deployed** to `C:\EverythingBox-app` (rebuilt
vehicle DLL + exe; no ABI bump so no mismatch churn). RetroPark changes merged to
`main` + pushed; EB submodule bumped. No AI attribution. Cores/ROMs never
committed.
