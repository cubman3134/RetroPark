# N64 in EverythingBox (HW-render, playable) — Design

**Status:** design, approved 2026-08-15. Implementation pending (writing-plans next).

## Context

Subsystem A (OpenGL host backend + user-selectable driven backend) and B1 (HW-render libretro cores via the
shim, readback path — Mupen64Plus-Next renders N64) are DONE. This increment makes **N64 games playable inside
EverythingBox** on the RetroPark backend: route N64 to RetroPark, render via the B1 HW-render shim, and add N64
controller input (the shim maps only NES buttons today). Render mirrors the NES/2b path; input is the real
addition. Spans two repos: the shim input change is RetroPark; the routing/self-heal/input-feed is EverythingBox
(`C:\Users\cubma\Project Goliath`, worktree flow).

## Goal

An N64 game launched on the RetroPark backend in EverythingBox renders (Mupen64Plus-Next / GLideN64, HW-render
→ readback → the driven composite path) AND is playable with a gamepad + keyboard. Deployed to
`C:\EverythingBox-app` and live-verified in-app. The NES software path stays byte-unchanged.

## Host backend (corrected)

N64 uses the **normal driven-backend resolution** — D3D11 by default, or the OpenGL host if the user selected
it (A's "RetroPark driven backend" setting) — exactly like any other driven core. **No N64-specific host
override.** Multiple GL contexts (Qt's, the shim's HW-render context, and — under the GL host — the host's)
coexist safely on the Qt thread: WGL's current-context is per-thread and every user calls `makeCurrent` before
its GL work. The shim's HW-render GL context is independent of the host regardless of which one composites the
readback frame.

## Non-goals (YAGNI)

- **Mupen64Plus-Next only** (parallel_n64 fallback deferred). The mechanism is core-agnostic.
- **NES input path unchanged** — the abstract-pad mapping is OR'd with the existing `keys[]` path, not a
  replacement.
- **Port 0 only** (single controller); no 4-player, no Rumble Pak / Transfer Pak, no per-game input remap UI.
- **No new EB deploy machinery** — reuse the established submodule-bump + worktree-build + targeted-copy flow.
- No B2 zero-copy (this rides B1 readback).

## Architecture

### 1. Shim: generic abstract-pad → libretro input (RetroPark repo)

`cores/libretro_shim/LibretroShim.cpp` currently answers `input_state_cb` for `RETRO_DEVICE_JOYPAD` from
`keys[]` (NES VK mapping). Add a **generic abstract-pad mapping** used ALONGSIDE `keys[]`:
- **JOYPAD buttons**: return `(abstract-pad bit) OR (keys[] NES bit)` for each libretro id. The abstract pad
  (`rp_input_state.pad_buttons`, `RP_PAD_*`) maps to the shared libretro ids: `RP_PAD_A→JOYPAD_A`,
  `RP_PAD_B→JOYPAD_B`, `RP_PAD_START→JOYPAD_START`, d-pad→`JOYPAD_UP/DOWN/LEFT/RIGHT`, `RP_PAD_L→JOYPAD_L`,
  `RP_PAD_R→JOYPAD_R`, and N64 **Z**→`JOYPAD_L2` (fed from `RP_AXIS_LEFT_TRIGGER` past a threshold, or a
  shoulder). For NES, RetroParkView feeds only `keys[]` (abstract pad zero) → `keys[]` wins → **NES byte-unchanged**.
- **ANALOG** (new — the shim doesn't answer `RETRO_DEVICE_ANALOG` today): on `RETRO_DEVICE_ANALOG`
  index `LEFT`, return `pad_axes[RP_AXIS_LEFT_X/Y]` (N64 analog stick); index `RIGHT`, return
  `pad_axes[RP_AXIS_RIGHT_X/Y]` (Mupen maps the right stick to the N64 C-buttons). Values are the abstract
  pad's signed −32768..32767, Y-sign per libretro (down = positive).

This is one uniform mapping serving N64 + any future system; it needs no per-core branching (a core polls the
ids it cares about). Reuse the abstract-pad constants from the ABI header (the Dolphin GC work defined `RP_PAD_*`
/ `RP_AXIS_*`).

### 2. RetroParkView: N64 routing + Mupen self-heal + abstract-pad feed (EverythingBox repo)

- **`native/src/core/EmuBackend.h`**: `retroParkSupportsSystem` += `"n64"` (so the Emulation picker offers
  RetroPark for N64, opt-in like NES).
- **`native/src/emu/RetroParkView.cpp` `openGame`**: for N64, use a **Mupen shim dir** —
  `<coresDir>/libretro_shim_n64` holding `LibretroShim.dll` + a `core.json` with
  `"libretro_core":"mupen64plus_next_libretro.dll"` + the Mupen DLL. Set it up by **self-healing** (mirror the
  fceumm self-heal, parameterized by core): copy EB's `LibretroShim.dll` + write the mupen `core.json` + copy
  EB's `mupen64plus_next_libretro.dll` (`CoreManager::corePath("mupen64plus_next")` — already present in EB)
  into the dir if missing/size-mismatched; graceful failure if EB lacks the core. Then
  `load_core(libretro_shim_n64)` + `load_content(rom)`. (Generalize the NES self-heal into a small helper taking
  `(shimSubdir, ebCoreId, coreDllName)`; NES calls it with `("libretro_shim","fceumm","fceumm_libretro.dll")`,
  N64 with `("libretro_shim_n64","mupen64plus_next","mupen64plus_next_libretro.dll")`. The helper is
  **idempotent and non-destructive**: `mkpath` the dir, copy `LibretroShim.dll` (from the build-staged
  `<coresDir>/libretro_shim`) and write `core.json` **only if absent** — so NES's build-staged `LibretroShim.dll`
  + `core.json` are never overwritten (NES byte-unchanged) — and (re)copy the libretro core DLL when missing or
  size-mismatched, exactly as today's fceumm self-heal.)
- **Input feed**: RetroParkView feeds `keys[]` for NES today; for N64 it feeds the **abstract pad** each tick
  from the shared gamepad + keyboard, reusing the gamepad→abstract-pad mapper from the Dolphin GC path
  (`RetroParkInput.h`'s presenting/abstract-pad path), via `rp_runtime_set_input(port 0, pad_buttons/pad_axes)`.
  (Keyboard may also map to the abstract pad for keyboard-only play.)

### 3. Build + deploy (EverythingBox repo)

- Bump `external/RetroPark` submodule to the shim-input commit. EB's ExternalProject rebuilds `LibretroShim`
  (now with N64 input); `retropark.lib` unchanged in behavior. Stage the `libretro_shim_n64` dir beside the exe
  (a build step, OR rely on RetroParkView's runtime self-heal like NES — self-heal is the robust choice; the
  build need only ensure `LibretroShim.dll` is current).
- Worktree off origin/main, wipe `retropark_ext-prefix` stamps (ABI unchanged but the submodule source
  changed), build Release, targeted-copy `EverythingBox.exe` (+ the `libretro_shim_n64` dir if build-staged)
  to `C:\EverythingBox-app`. No new runtime DLLs beyond the (already-present) Mupen core.

## Data flow (one N64 frame in EB)

```
RetroParkView present() [D3D11 or GL host runtime]
  -> shim run_frame -> make shim GL ctx current -> mupen retro_run
       -> GLideN64 renders N64 3D into the shim FBO (OpenGL)
       -> video_cb(RETRO_HW_FRAME_BUFFER_VALID) -> HwRenderGL readback+flip -> CPU RGBA -> driven video_refresh
  -> runtime composites -> QImage -> Qt paint
input each tick: gamepad/keyboard -> abstract pad (RP_PAD_*/RP_AXIS_*) -> rp_runtime_set_input(0)
  -> shim input_state_cb maps -> mupen JOYPAD + ANALOG
```

## Error handling

- EB lacks `mupen64plus_next` → self-heal fails gracefully with a clear message (the fceumm-absent pattern);
  the user can launch N64 on the libretro backend once to download it, or EB downloads it on demand.
- Shim GL/HW-render setup fails (rare) → `load_content` errors; RetroParkView shows the failure (existing path).
- A misbehaving N64 frame → B1's clamp/gate handles it (no crash).
- NES and all non-N64 driven cores: unaffected (abstract pad zero → `keys[]` path unchanged; ANALOG only
  answered when a core polls it, which NES never does).

## Testing

- **Shim input (RetroPark, unit)**: a small doctest for the abstract-pad → libretro mapper — assert
  `RP_PAD_A→JOYPAD_A`, d-pad, `RP_AXIS_LEFT→ANALOG LEFT`, Z→L2, and that a zero abstract pad + set `keys[]`
  still yields the NES bits (OR semantics; NES unregressed). Mutation-worthy (the mapping is pure).
- **RetroPark N64 e2e (existing, extended)**: the B1 gated N64 e2e can hold a strong input and assert the input
  poll count / a state change, proving the abstract pad reaches the core (device-independent, like the Dolphin
  input proof).
- **EB live (the load-bearing gate)**: launch EverythingBox (worktree build), pick RetroPark for an N64 game,
  confirm it **renders** (capture a frame — the Qt/WGL coexistence is exercised for real now) and **responds to
  input** (a held direction visibly moves something / the input reaches the core). Then the same on the deployed
  app. This is the real verification; the in-app N64 render+input is the deliverable's proof.

## File-structure summary

| File | Repo | Responsibility |
|------|------|----------------|
| `cores/libretro_shim/LibretroShim.cpp` | RetroPark | generic abstract-pad → libretro JOYPAD+ANALOG (OR keys[]) |
| `tests/test_shim_input.cpp` (or extend) | RetroPark | unit test the mapping + NES-OR invariant |
| `native/src/core/EmuBackend.h` | EB | `retroParkSupportsSystem` += "n64" |
| `native/src/emu/RetroParkView.cpp` | EB | N64 Mupen self-heal (parameterized helper) + abstract-pad feed |
| EB submodule bump + deploy | EB | worktree build + targeted redeploy + live-verify |

## Open follow-ups (deferred)

- parallel_n64 fallback; other HW systems (PS1-HW, Dreamcast, PSP/GLES via ANGLE).
- 4-player / Rumble Pak / per-game input remap.
- Unifying the NES input path onto the abstract pad (optional cleanup; not now, to avoid regressing NES).
- B2 zero-copy GL-to-GL for HW cores.
