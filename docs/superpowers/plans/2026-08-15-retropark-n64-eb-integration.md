# N64 in EverythingBox (HW-render, playable) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** N64 games launch on the RetroPark backend in EverythingBox, render via the B1 HW-render shim
(Mupen64Plus-Next/GLideN64), and are playable with a gamepad + keyboard. Deployed + live-verified. NES SW path
byte-unchanged.

**Architecture:** Shim gains a generic abstract-pad → libretro JOYPAD+ANALOG mapping OR'd with the existing
`keys[]` path (RetroPark repo). EverythingBox routes N64 to RetroPark (mirror the NES self-heal, parameterized
for Mupen) and feeds the abstract pad for N64 (reusing the GC feed), then deploys.

**Tech Stack:** C++17, the B1 libretro shim, libretro input API, Qt/CMake (EB), doctest, worktree deploy.

**Reference spec:** `docs/superpowers/specs/2026-08-15-retropark-n64-eb-integration-design.md`.

## Global Constraints

- **NES/SW input byte-unchanged.** The abstract-pad mapping is OR'd with `keys[]` (NES feeds only `keys[]`;
  its abstract pad is zero → `keys[]` wins). ANALOG is only answered when a core polls it (NES never does).
- **N64 uses the normal driven-backend resolution** (D3D11 default / GL host if selected) — no host special-case.
- **Port 0 only**; no rumble, no 4-player.
- **No ABI change** (input rides the existing `rp_input_state` abstract pad + `keys[]`).
- **Cores/ROMs never committed** (Mupen core is git-ignored; EB already has it deployed).
- No AI attribution. RetroPark: merge to main + push at task end. EB: worktree off origin/main, targeted deploy.

---

### Task 1: Shim — generic abstract-pad → libretro JOYPAD+ANALOG input (RetroPark repo)

**Files:**
- Modify: `cores/libretro_shim/LibretroShim.cpp` (`input_state_cb`)
- Test: `tests/test_shim_input.cpp` (new) + extend the gated N64 e2e to feed input

**Interfaces:**
- Consumes: `rp_input_state` (`keys[256]`, `pad_buttons`, `pad_axes[8]`), `RP_PAD_*`/`RP_AXIS_*` (retropark_abi.h),
  libretro `RETRO_DEVICE_JOYPAD`/`RETRO_DEVICE_ANALOG` ids.
- Produces: an `input_state_cb` that serves N64 (JOYPAD+ANALOG from the abstract pad) and NES (keys[]) uniformly.

- [ ] **Step 1: Replace `input_state_cb`** with the OR'd + ANALOG version. GL/abstract-pad constants come from
  `retropark_abi.h` (already included via retropark.h); libretro ids from libretro.h.

```cpp
int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    if ((port != 0 && port != 1) || !g) return 0;
    const rp_input_state& in = g->input[port];

    if (device == RETRO_DEVICE_ANALOG) {
        // N64 analog stick = LEFT; C-buttons = RIGHT stick (Mupen's default). The abstract pad uses Y-UP
        // positive (the Dolphin GC contract); libretro analog uses Y-DOWN positive -> negate Y.
        int16_t ax = 0, ay = 0;
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)  { ax = in.pad_axes[RP_AXIS_LEFT_X];  ay = in.pad_axes[RP_AXIS_LEFT_Y]; }
        else if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) { ax = in.pad_axes[RP_AXIS_RIGHT_X]; ay = in.pad_axes[RP_AXIS_RIGHT_Y]; }
        else return 0;
        if (id == RETRO_DEVICE_ID_ANALOG_X) return ax;
        if (id == RETRO_DEVICE_ID_ANALOG_Y) return (int16_t)(-ay);
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD) return 0;

    // JOYPAD: OR the abstract pad (pad_buttons/RP_PAD_*) with the existing keys[] NES map. NES feeds only
    // keys[] (pad_buttons==0 -> keys[] wins, byte-unchanged); N64 feeds the abstract pad.
    auto pad = [&](int b){ return (in.pad_buttons & (1u << b)) != 0; };
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return (in.keys[VK_UP]     || pad(RP_PAD_DPAD_UP))    ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (in.keys[VK_DOWN]   || pad(RP_PAD_DPAD_DOWN))  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (in.keys[VK_LEFT]   || pad(RP_PAD_DPAD_LEFT))  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (in.keys[VK_RIGHT]  || pad(RP_PAD_DPAD_RIGHT)) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:      return (in.keys['X']       || pad(RP_PAD_A))          ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return (in.keys['Z']       || pad(RP_PAD_B))          ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return (in.keys[VK_RETURN] || pad(RP_PAD_START))      ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (in.keys[VK_SHIFT]  || pad(RP_PAD_SELECT))     ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_L:      return pad(RP_PAD_L) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_R:      return pad(RP_PAD_R) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_L2:     return (in.pad_axes[RP_AXIS_LEFT_TRIGGER] > 8192) ? 1 : 0; // N64 Z
        default: return 0;
    }
}
```

- [ ] **Step 2: Unit test** (`tests/test_shim_input.cpp`). The mapper is `static` inside the shim TU, so test
  the PURE mapping by extracting the switch into a tiny free function `int16_t shim_map_input(const
  rp_input_state&, unsigned device, unsigned index, unsigned id)` that `input_state_cb` calls (keeps the cb a
  thin wrapper), and unit-test THAT (no DLL/GL). Assert:
  - `RP_PAD_A` set → `JOYPAD_A` returns 1; d-pad bits → the 4 dirs; `RP_PAD_L/R` → L/R.
  - `RP_AXIS_LEFT_TRIGGER` past threshold → `JOYPAD_L2` (Z).
  - `RP_AXIS_LEFT_X=30000` → `ANALOG LEFT X` returns 30000; `RP_AXIS_LEFT_Y=30000` → `ANALOG LEFT Y` returns
    -30000 (Y negated); RIGHT index → right axes.
  - **NES-OR invariant:** `pad_buttons=0` + `keys['X']=1` → `JOYPAD_A` still 1 (keys[] path intact); a fully
    zero `rp_input_state` → all 0.

```cpp
// tests/test_shim_input.cpp
#include <doctest/doctest.h>
#include <retropark/retropark_abi.h>
#include "../cores/libretro_shim/ShimInput.h"   // the extracted shim_map_input (see Step 1 refactor)
#include "libretro.h"                            // (add external/libretro to the test include path if needed)
using namespace rp; // if shim_map_input is namespaced; else drop
TEST_CASE("shim input: abstract pad -> libretro JOYPAD+ANALOG, NES keys[] OR intact") {
    rp_input_state in{};
    // NES-OR: keys only
    in.keys['X'] = 1;
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1);
    in = rp_input_state{};
    // abstract pad buttons
    in.pad_buttons = (1u<<RP_PAD_A) | (1u<<RP_PAD_DPAD_LEFT);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 1);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) == 1);
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) == 0);
    // Z from left trigger
    in = rp_input_state{}; in.pad_axes[RP_AXIS_LEFT_TRIGGER] = 20000;
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) == 1);
    // analog (Y negated)
    in = rp_input_state{}; in.pad_axes[RP_AXIS_LEFT_X] = 30000; in.pad_axes[RP_AXIS_LEFT_Y] = 30000;
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) == 30000);
    CHECK(shim_map_input(in, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) == -30000);
    // all zero -> nothing
    in = rp_input_state{};
    CHECK(shim_map_input(in, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 0);
}
```

  Refactor note: put `shim_map_input` in a new tiny header `cores/libretro_shim/ShimInput.h` (pure, includes
  only `retropark_abi.h` + `libretro.h`), included by both `LibretroShim.cpp` and the test. Add
  `tests/test_shim_input.cpp` to `retropark_tests` (+ the `external/libretro` include path if not already there).

- [ ] **Step 3: Extend the gated N64 e2e** (`tests/test_hwrender_n64_e2e.cpp`) to prove input reaches the core:
  before the pump loop, `rp_input_state held{}; held.pad_buttons = (1u<<RP_PAD_START); held.pad_axes[RP_AXIS_LEFT_X] = -32767;
  rp_runtime_set_input(rt, 0, &held);` and after, assert `rp_runtime_input_poll_count(rt) > 0` (device-independent
  proof the shim polled host input, mirroring the Dolphin input gate). (Rendering assertions unchanged.)

- [ ] **Step 4: Build + run** — full suite green; `-tc="shim input*"` passes; gated `RP_RUN_N64=1` still renders
  Banjo-Tooie + now `input_poll_count>0`. **Verify NES unregressed:** run the FCEUmm gated e2e if present.
- [ ] **Step 5: Commit + push** (`feat(shim): generic abstract-pad -> libretro JOYPAD+ANALOG input (N64)`).
  Record the commit SHA — Task 2 bumps EB's submodule to it.

---

### Task 2: EverythingBox — route N64 to RetroPark + Mupen self-heal + abstract-pad feed

**Repo:** EverythingBox (`C:\Users\cubma\Project Goliath`). **Worktree off origin/main** (see
[[goliath-tree-is-shared]]); do not edit the shared tree.

**Files:** `native/src/core/EmuBackend.h`; `native/src/emu/RetroParkView.cpp`; `external/RetroPark` (submodule).

- [ ] **Step 1: Worktree + submodule bump.** Create a worktree off current origin/main; `git submodule update
  --init external/RetroPark`; `git -C external/RetroPark fetch && checkout <Task-1 SHA>`. Wipe
  `build/retropark_ext-prefix` (submodule source changed — the shim rebuilds; ABI unchanged so no vehicle work).
- [ ] **Step 2: `retroParkSupportsSystem` += "n64"** (`native/src/core/EmuBackend.h`):
  `return systemId == "nes" || systemId == "gc" || systemId == "n64";`. Leave `retroParkSystemIsPresenting`
  unchanged (n64 is driven, returns false). Update the doc comment (add the "n64" line: HW-render libretro via
  Mupen64Plus-Next, driven, CPU pixels after readback).
- [ ] **Step 3: Parameterize the shim self-heal** in `RetroParkView.cpp`. Extract the current fceumm self-heal
  (the block that copies EB's fceumm into `<coresDir>/libretro_shim`) into a helper, e.g.:
  `QString ensureShimDir(const QString& subdir, const QString& ebCoreId, const QString& coreDllName, QString* err)`
  returning the shim dir path (or empty on failure). It must be **idempotent + non-destructive**: `mkpath` the
  dir; if `LibretroShim.dll` is absent, copy it from the build-staged `<coresDir>/libretro_shim/LibretroShim.dll`;
  if `core.json` is absent, WRITE `{ ... "libretro_core":"<coreDllName>" }` (do NOT overwrite an existing one —
  protects the NES build-staged core.json); (re)copy `CoreManager::corePath(ebCoreId)` -> `<dir>/<coreDllName>`
  when missing/size-mismatched (the existing fceumm logic). NES calls
  `ensureShimDir("libretro_shim","fceumm","fceumm_libretro.dll",…)` (behaviour identical to today — dir + shim +
  core.json already staged, so it only self-heals fceumm); N64 calls
  `ensureShimDir("libretro_shim_n64","mupen64plus_next","mupen64plus_next_libretro.dll",…)` (creates the dir +
  shim copy + mupen core.json + mupen DLL). In `openGame`, pick the subdir/core by `systemId` (nes→fceumm,
  n64→mupen64plus_next), then `load_core(dir)` + `load_content(rom)`.
- [ ] **Step 4: Feed the abstract pad for N64.** In `RetroParkView::feedInput()`, the GC path (presenting)
  already builds the abstract pad (`pad_buttons` from keyboard `padKeyButtons_` + `sharedPad_` via
  `rpinput::rpJoyBitForRetroPad`, `pad_axes` from `sharedPad_->axis`, triggers) and `rp_runtime_set_input(rt_,0,&in)`.
  Route N64 through that SAME build+feed instead of the `keys[]` path: change the branch so the abstract-pad
  feed runs when `presenting_ || systemId_ == "n64"` (add a small `retroParkSystemUsesGamepad(systemId)` helper
  in EmuBackend.h if cleaner — true for gc + n64). N64 also still fills `keys[]` (keyboard) harmlessly for the
  shim's OR, but the abstract pad is what carries analog + the gamepad. The NES branch (keys[] only) is untouched.
- [ ] **Step 5: Build Release** (worktree). Redirect to a file, check `$?`. Confirm 0 errors + EverythingBox.exe
  relinks + `retropark.lib`/LibretroShim rebuild from the bumped submodule (wipe stamps if a stale-lib LNK1181
  appears — see [[retropark-eb-integration]]).
- [ ] **Step 6: Live gate (worktree build).** Launch (`EB_UITEST` / normal), pick RetroPark for an N64 game
  (Banjo-Tooie), confirm it **renders** in-app (capture a frame — the Qt/WGL coexistence is real here) AND
  **responds to input** (a held stick/button reaches the game; or assert via the game visibly reacting). Confirm
  NES still launches + plays on RetroPark (regression).
- [ ] **Step 7: Commit** the submodule bump + EB changes in the worktree; push to main
  (`feat: N64 playable on the RetroPark backend (HW-render + input)`).

---

### Task 3: Deploy to `C:\EverythingBox-app` + verify

- [ ] **Step 1:** Targeted-copy `EverythingBox.exe` (+`.pdb`) from the worktree `build/Release` to
  `C:\EverythingBox-app` — **NOT `robocopy /MIR`**. The Mupen core is already deployed; the `libretro_shim_n64`
  dir is created by RetroParkView's runtime self-heal on first N64 launch (no build-staged dir needed), so no
  new files beyond the exe. (If you chose to build-stage `libretro_shim_n64`, copy that dir too.)
- [ ] **Step 2:** Launch the deployed app, play an N64 game on RetroPark — confirm render + input. Confirm a NES
  game still works.
- [ ] **Step 3:** Update memory ([[retropark-eb-integration]] / [[retropark-project]]) with N64 playable in EB.

---

## Self-Review notes

- **Spec coverage:** shim input (Task 1) / EB routing+self-heal+input-feed (Task 2) / deploy (Task 3) — all mapped.
- **NES byte-unchanged:** the shim ORs the abstract pad (zero for NES) with `keys[]`; the self-heal helper never
  overwrites NES's staged `core.json`/`LibretroShim.dll`; the `feedInput` NES branch is untouched.
- **No ABI change** — input rides the existing `rp_input_state`.
- **The load-bearing verification is Task 2 Step 6 / Task 3 Step 2** — in-app N64 render+input, where the shim's
  HW-render GL context coexists with Qt's GL for real. If that surfaces a Qt/WGL issue, it shows here (debug,
  don't stub).
- **Type consistency:** `shim_map_input` is the single mapping home (shim + test share `ShimInput.h`);
  `rp_runtime_set_input` carries the abstract pad the shim reads; N64 Z = left trigger past threshold both in the
  shim (reads `RP_AXIS_LEFT_TRIGGER`) and the EB feed (fills it).
