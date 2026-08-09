# RetroPark — Slice M Design (Dolphin input, host-owned, keyboard + analog gamepad)

**Date:** 2026-08-08
**Status:** Approved (design)
**Scope:** Make GameCube games playable through the `dolphin_present` core. RetroPark's host feeds
controller input (keyboard + analog gamepad); the vehicle injects it into Dolphin's emulated GC
controller (port 0) via Dolphin's `SetInputOverrideFunction`. Host-owned — the presenting-core analogue
of the driven/netplay input path. **No core-ABI change** (ABI stays v5).

---

## 0. Context and goal

Slices I–L made `dolphin_present` render + play audio through RetroPark's Runtime, but it takes **no
input** — you can watch and hear Billy Hatcher's attract loop but not play it. RetroPark already has a
host-owned input path from Slice G (netplay): `rp_input_state` (`keys[256]` + an abstract pad
`pad_axes[8]`/`pad_buttons`), the `input_state` host callback (`Runtime::on_input` serves `input_[port]`),
and `rp_runtime_set_input(port, state)`. The windowed harness fills `keys[]` from `GetAsyncKeyState`; the
libretro shim maps those keys to libretro buttons for driven cores. The abstract pad fields exist but are
undefined/unused.

Slice M closes the gap for Dolphin. Dolphin exposes exactly the hook we need: an emulated controller's
`SetInputOverrideFunction(std::function<std::optional<ControlState>(group, control, state)>)` — the same
mechanism Dolphin's scripting uses to override inputs. The vehicle registers an override on the GC pad
(port 0) that reads `rp_host.input_state` and returns per-control values. This is host-owned (remap /
netplay-ready), works despite the hidden off-screen window (which can't hold keyboard focus), and takes
a real gamepad for true analog control of the GC stick.

### Decisions

| Decision | Choice |
|---|---|
| Ownership | **Host-owned.** Host feeds `rp_input_state`; the vehicle injects via `SetInputOverrideFunction`. Not Dolphin reading its own devices. |
| Sources | **Keyboard + analog gamepad.** Keyboard → `keys[]` (existing). Gamepad → the abstract pad (`pad_axes`/`pad_buttons`) via **XInput** in the harness (Windows-only, no new dependency — `xinput` ships in the Windows SDK). |
| Abstract pad | **Define the layout** as named constants in the ABI header (bits of `pad_buttons`, indices of `pad_axes`). The `rp_input_state` struct is unchanged → **no ABI version bump**. |
| Injection | `Pad::GetConfig()->GetController(0)->SetInputOverrideFunction(...)` after boot; the override maps `rp_input_state` → GC controls (analog stick/triggers + digital buttons). Seed `MAIN_SI_DEVICE[0] = Standard Controller` so port 0 is polled. Cleared on stop. |
| Ports | **Port 0 only** (single controller) — enough to fully play the game. 4 ports deferred. |

---

## 1. The abstract-pad layout (ABI header, no version bump)

`rp_input_state.pad_buttons` (uint16) bit flags and `pad_axes[8]` (int16) indices, added as `#define`/enum
constants in `include/retropark/retropark_abi.h`. Generic (RetroPad/XInput-friendly); cores map to their
own controls. Sticks range −32768..32767 (Y **up = positive**); triggers 0..32767.

```
pad_buttons bits:  0 A   1 B   2 X   3 Y   4 L(shoulder)   5 R(shoulder)
                   6 SELECT   7 START   8 L3   9 R3
                   10 DPAD_UP  11 DPAD_DOWN  12 DPAD_LEFT  13 DPAD_RIGHT   14 GUIDE
pad_axes indices:  0 LEFT_X  1 LEFT_Y  2 RIGHT_X  3 RIGHT_Y  4 LEFT_TRIGGER  5 RIGHT_TRIGGER
```

The struct layout and `RETROPARK_ABI_VERSION` (5) do not change; these constants only name the meaning of
already-shipping fields. Existing consumers (harness keyboard path, libretro shim) are unaffected.

## 2. Components

### Harness — read a gamepad (`harness/windowed/main.cpp`)
- Poll **XInput** (`XInputGetState`, controller 0) each frame; translate `XINPUT_STATE` into the abstract
  pad: `wButtons` → `pad_buttons` (via the layout above), `sThumbLX/LY/RX/RY` → `pad_axes[0..3]`,
  `bLeftTrigger/bRightTrigger` (0..255) → `pad_axes[4..5]` scaled to 0..32767. Left thumb Y is negated so
  up is positive (XInput reports up as positive already; document the sign). Keep the existing keyboard
  `keys[]` fill. Link `xinput` (Windows SDK).
- Feed input for the **presenting** path too: ensure `rp_runtime_set_input(port, &state)` is called each
  present iteration for the Dolphin core (it is already called for driven cores; extend/confirm for
  presenting).

### Vehicle — inject into Dolphin (`external/dolphin/.../rp_dolphin.cpp`, patch)
- **Enable port 0 as a GC controller.** Seed `Config::MAIN_SI_DEVICE[0] = SerialInterface::SIDEVICE_GC_CONTROLLER`
  before boot so the SI polls a standard controller (else the override never fires).
- **Register the override after boot.** Once `Pad::IsInitialized()`, get the port-0 controller
  (`Pad::GetConfig()->GetController(0)`, an `EmulatedController`) and call `SetInputOverrideFunction` with
  a lambda. The lambda, for a given `(group_name, control_name)`:
  1. Reads the freshest host input: `rp_host.input_state(rp_host.host, 0, &state)` (thread-safe — it locks
     `Runtime::input_mtx_`; called on Dolphin's CPU thread during SI poll). No-op passthrough (`return
     std::nullopt`) when no host is set (Slice-J direct-C-API mode) so behavior is unchanged there.
  2. Maps to a `ControlState` (0.0..1.0 for buttons/one-directional; signed for stick halves):
     - **Control Stick** group ← `pad_axes` LEFT_X/LEFT_Y (analog), with keyboard arrows as full-deflection
       fallback. **C-Stick** ← RIGHT_X/RIGHT_Y. **Triggers** L/R ← LEFT/RIGHT_TRIGGER analog (+ digital
       shoulder buttons as full press). **Buttons** A/B/X/Y/Z/Start and **D-Pad** ← `pad_buttons` (with a
       default keyboard map as fallback).
     - GC has no Select; map generic SELECT (or a chosen key) → GC **Z**.
  3. Returns `std::nullopt` for controls we don't drive (Dolphin falls back to its unmapped physical device
     = neutral).
- **Clear on stop.** `ClearInputOverrideFunction()` (and drop the host ref) in `dp_stop`/before teardown.
- The exact GC group/control names (`"Buttons"`, `"Control Stick"`, `"C-Stick"`, `"Triggers"`, `"D-Pad"`
  and their control names A/B/X/Y/Z/Start/Up/Down/Left/Right/L/R) are read from `Core/HW/GCPadEmu.cpp` when
  writing the plan; the override matches on them.

### RetroPark Runtime — an input-poll counter (small change)
`on_input` / `set_input` / `input_[2]` (Slice G) already serve the `input_state` callback. Add a single
counter: `Runtime::on_input` increments an `input_polls_` count each time a core pulls input, exposed via a
new C API `uint64_t rp_runtime_input_poll_count(rp_runtime*)`. This is the device-independent plumbing
proof — every time the vehicle's override calls `rp_host.input_state`, it routes through `on_input`, so a
non-zero count proves Dolphin is pulling host input each poll through the ABI. Reset on `unload_core`.
(Make the counter atomic like the Slice-L audio counters — the override calls `on_input` from Dolphin's
CPU thread while the test reads the count on the host thread.)

## 3. Data flow (per input poll)

```
Harness each frame: keyboard (keys[]) + XInput (pad_axes[]/pad_buttons)
   -> rp_runtime_set_input(0, state)         [Runtime stores input_[0] under input_mtx_]
Dolphin CPU thread polls the GC pad (SI):
   EmulatedController::GetState -> our override(group, control, state)
      -> rp_host.input_state(host, 0, &s)    [locks input_mtx_, copies input_[0]]
         -> map s -> optional<ControlState>   -> GC pad status -> game logic
```

## 4. Error handling / robustness

- No gamepad connected → `XInputGetState` returns an error; the harness leaves the pad fields neutral and
  keyboard still works. No crash.
- Override thread-safety: it only calls `rp_host.input_state` (mutex-guarded copy) and pure mapping; safe
  on the CPU thread. When `rp_host.input_state` is null (direct C-API mode) it returns `std::nullopt` for
  every control → Dolphin behaves exactly as before Slice M.
- Registration timing: guarded on `Pad::IsInitialized()` so the override is installed only once the pad
  config exists; cleared on stop before Dolphin tears the controller down.

## 5. Testing

- **Plumbing (device-independent, reliable) — the primary automated proof.** `rp_runtime_input_poll_count`
  proves Dolphin pulled host input through the ABI: every SI poll runs the override, which calls
  `rp_host.input_state` → `Runtime::on_input` → the counter. The gated e2e boots Billy Hatcher, feeds a
  **held** input (e.g. full Control Stick + A) via `rp_runtime_set_input`, drives `present()`, and asserts
  the poll count > 0. Parallels the audio slice's device-independent stats. Opt-in `RP_RUN_DOLPHIN=1`.
- **Functional (gated, best-effort).** Hold a strong input for a window and assert the rendered frame
  stream **diverges** from the neutral baseline captured earlier in the run — evidence the game reacts.
  Softer (a real game's reaction is timing-dependent); a failure here that the plumbing test passes is
  reported, not fatal to the slice.
- **Harness (human proof).** You play Billy Hatcher in the windowed harness with an analog gamepad (real
  stick control) or the keyboard.
- **Regression.** Full A–L suite green; the driven/libretro keyboard input path and the Slice-J direct
  C-API mode (override returns `nullopt`) are unchanged.

## 6. Scope

**In Slice M:** the abstract-pad layout constants (no ABI bump); harness XInput gamepad → abstract pad;
the vehicle's `SetInputOverrideFunction` mapping (analog stick/triggers + digital buttons + keyboard
fallback) on GC port 0; the SI-device seed; the plumbing counter + gated e2e; playable harness.

**Out (later):** 4 controller ports, rumble/force-feedback, Wii/Wiimote + MotionPlus/IR input, per-user
remappable bindings + an in-app config UI, Dolphin's own input config, GC keyboard-controller / GBA-link
peripherals, SDL gamepad backend (XInput-only for now), analog dead-zone/calibration tuning UI.

**The single provable claim:** *RetroPark's host input reaches Dolphin's emulated GameCube controller
through `rp_host.input_state` + `SetInputOverrideFunction`, driven through `rp_core_abi` — you play a real
GameCube game with an analog gamepad or the keyboard, host-owned, no libretro — proven by a gated
device-independent input-plumbing assertion and the game responding in the harness.*

## 7. Repo additions

```
include/retropark/retropark_abi.h                        # abstract-pad layout constants (no version bump)
include/retropark/retropark.h                            # + rp_runtime_input_poll_count
src/runtime/Runtime.h, src/runtime/Runtime.cpp           # input_polls_ counter + on_input increment + C API
harness/windowed/main.cpp                                # XInput gamepad -> pad_axes/pad_buttons; feed presenting path
external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp # SI-device seed + SetInputOverrideFunction mapping (patch)
tests/test_dolphin_core_e2e.cpp                          # + gated input-plumbing assertion (held input -> poll count>0)
docs/patches/dolphin-external-present.patch              # refreshed with the input injection changes
```
