# RetroPark Slice M — Dolphin Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make GameCube games playable through `dolphin_present`: RetroPark's host feeds controller input (keyboard + analog XInput gamepad), and the vehicle injects it into Dolphin's emulated GC controller (port 0) via `SetInputOverrideFunction`.

**Architecture:** Host-owned input, mirroring the driven/netplay path. The harness fills `rp_input_state` (keyboard `keys[]` + abstract pad `pad_axes[]`/`pad_buttons` from XInput) and calls `rp_runtime_set_input`. Dolphin's CPU thread polls its GC pad each frame; our override reads `rp_host.input_state` (→ `Runtime::on_input`) and returns per-control values. A device-independent `rp_runtime_input_poll_count` proves the path fired.

**Tech Stack:** C++17, Dolphin (tag 2606) `ControllerEmu`/`GCPadEmu` `SetInputOverrideFunction`, Win32 XInput, RetroPark Runtime, doctest, MSBuild (Dolphin DLL) + CMake/MSBuild (RetroPark).

## Global Constraints

- **No AI attribution** in any commit message or PR body. Conventional prefixes (`feat:`/`fix:`/`docs:`).
- **ABI stays v5** — do NOT change `RETROPARK_ABI_VERSION` or the `rp_input_state` struct layout. Slice M only ADDS named constants for existing fields and a new C API function.
- **`external/dolphin` is git-ignored** — Dolphin-side changes are captured to `docs/patches/dolphin-external-present.patch`. Regenerate stdout-only: `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null` (stderr CRLF warnings corrupt the file otherwise).
- **Never commit cores or ROMs.** Billy Hatcher ROM: `C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz`.
- **Dolphin DLL relink recipe** (Task 3): `MSBuild RetroParkDolphin.vcxproj -p:Configuration=Release -p:Platform=x64 -p:SolutionDir="…\external\dolphin\Source\\" -p:BuildProjectReferences=false -m -v:minimal -nologo` via **PowerShell** (a plain rebuild hard-fails at the sibling `glslang` project's VS17/VS18 cmake-cache mismatch; `-p:BuildProjectReferences=false` recompiles only `rp_dolphin.cpp` and relinks). Use PowerShell for `dumpbin` too. AfterBuild copies `dolphin_present.dll` + `cores/dolphin_present/core.json` into `external/dolphin/Binary/x64/`.
- **RetroPark build:** `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`. Full suite: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe` → `100 passed | 0 failed`. The Dolphin e2e is opt-in `RP_RUN_DOLPHIN=1`, WARN-skips without GPU/DLL/ROM.

---

### Task 1: RetroPark plumbing — abstract-pad constants + input-poll counter + C API

RetroPark-side, no Dolphin. Defines the shared pad layout and the device-independent proof counter.

**Files:**
- Modify: `include/retropark/retropark_abi.h` (add pad-layout constants near `rp_input_state`)
- Modify: `include/retropark/retropark.h` (declare `rp_runtime_input_poll_count`)
- Modify: `src/runtime/Runtime.h` (add `input_polls_` member + accessor), `src/runtime/Runtime.cpp` (increment in `on_input`, reset in `unload_core`, implement C API)
- Test: `tests/test_input_ports.cpp` (assert the counter increments and resets)

**Interfaces:**
- Produces (Task 2 & 3 consume): the `RP_PAD_*` bit-index and `RP_AXIS_*` index constants; `uint64_t rp_runtime_input_poll_count(rp_runtime* rt)`.

- [ ] **Step 1: Add the pad-layout constants.** In `include/retropark/retropark_abi.h`, immediately after the `rp_input_state` struct definition, add:

```c
/* rp_input_state.pad_buttons bit indices (generic abstract pad; a bit is (1u << RP_PAD_x)). */
#define RP_PAD_A          0
#define RP_PAD_B          1
#define RP_PAD_X          2
#define RP_PAD_Y          3
#define RP_PAD_L          4   /* left shoulder (digital) */
#define RP_PAD_R          5   /* right shoulder (digital) */
#define RP_PAD_SELECT     6
#define RP_PAD_START      7
#define RP_PAD_L3         8   /* left stick click */
#define RP_PAD_R3         9   /* right stick click */
#define RP_PAD_DPAD_UP    10
#define RP_PAD_DPAD_DOWN  11
#define RP_PAD_DPAD_LEFT  12
#define RP_PAD_DPAD_RIGHT 13
#define RP_PAD_GUIDE      14
/* rp_input_state.pad_axes[] indices. Sticks -32768..32767 (Y up = positive); triggers 0..32767. */
#define RP_AXIS_LEFT_X        0
#define RP_AXIS_LEFT_Y        1
#define RP_AXIS_RIGHT_X       2
#define RP_AXIS_RIGHT_Y       3
#define RP_AXIS_LEFT_TRIGGER  4
#define RP_AXIS_RIGHT_TRIGGER 5
```

- [ ] **Step 2: Write the failing counter test.** In `tests/test_input_ports.cpp`, add a new test case at the end:

```cpp
TEST_CASE("runtime: input poll counter tracks on_input calls") {
    Runtime rt(RP_GFX_NONE, nullptr);
    auto* h = reinterpret_cast<rp_runtime*>(&rt);
    CHECK(rp_runtime_input_poll_count(h) == 0);
    rp_input_state out{};
    rt.on_input(0, &out);
    rt.on_input(0, &out);
    rt.on_input(1, &out);
    CHECK(rp_runtime_input_poll_count(h) == 3);   // every pull counts, any port
    // Also assert the layout constants are the distinct values consumers rely on.
    CHECK(RP_PAD_A == 0);
    CHECK(RP_PAD_START == 7);
    CHECK(RP_AXIS_LEFT_Y == 1);
    rt.unload_core();                              // resets the counter (no core loaded is fine)
    CHECK(rp_runtime_input_poll_count(h) == 0);
}
```

- [ ] **Step 3: Run it, verify it fails to compile/link.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests`
Expected: FAIL — `rp_runtime_input_poll_count` undeclared.

- [ ] **Step 4: Declare the C API.** In `include/retropark/retropark.h`, near `rp_runtime_set_input`, add:

```c
/* Number of times a core has pulled host input via the input_state callback since the last core load.
   Device-independent; used to prove a presenting core (e.g. Dolphin) is polling host input. */
uint64_t    rp_runtime_input_poll_count(rp_runtime* rt);
```

- [ ] **Step 5: Add the counter member.** In `src/runtime/Runtime.h`, ensure `#include <atomic>` is present (it is, from Slice L), and add near the audio counters:

```cpp
    std::atomic<uint64_t> input_polls_{0};
```

- [ ] **Step 6: Increment, reset, and implement the C API.** In `src/runtime/Runtime.cpp`:
  - In `Runtime::on_input`, add the increment as the first line of the body:

```cpp
void Runtime::on_input(uint32_t port, rp_input_state* out) {
    input_polls_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_[port & 1u];               // clamp to {0,1}
}
```

  - In `Runtime::unload_core`, alongside the other counter resets (near `audio_frames_ = 0;`), add:

```cpp
    input_polls_.store(0, std::memory_order_relaxed);
```

  - Add the C API next to `rp_runtime_set_input`:

```cpp
uint64_t rp_runtime_input_poll_count(rp_runtime* rt) {
    return reinterpret_cast<Runtime*>(rt)->input_polls_.load(std::memory_order_relaxed);
}
```

- [ ] **Step 7: Build and run the full suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `101 passed | 0 failed` (new test passes; nothing regresses).

- [ ] **Step 8: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add include/retropark/retropark_abi.h include/retropark/retropark.h src/runtime/Runtime.h src/runtime/Runtime.cpp tests/test_input_ports.cpp && git commit -m "feat(input): abstract-pad layout constants + rp_runtime_input_poll_count (device-independent input-poll proof)"
```

---

### Task 2: Harness — XInput gamepad → abstract pad; feed single-player input

Makes a real controller (and the existing keyboard) drive `rp_input_state`, and feeds it every present iteration — single-player input is currently NOT wired (only netplay reads input today), so this is what makes Dolphin (and driven cores) controllable in the harness.

**Files:**
- Create: `harness/windowed/xinput_map.h` (pure `XINPUT_GAMEPAD` → `rp_input_state` mapping)
- Modify: `harness/windowed/main.cpp` (poll XInput in `read_local_input`; feed `set_input` in the single-player loop; link xinput)
- Modify: `harness/windowed/CMakeLists.txt` (link `xinput`)
- Test: `tests/test_xinput_map.cpp` (new; unit-test the pure mapping) + register in `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `RP_PAD_*` / `RP_AXIS_*` (Task 1); `rp_runtime_set_input` (existing).
- Produces: `void xinput_to_pad(const XINPUT_GAMEPAD& gp, rp_input_state& s)` — sets `s.pad_buttons` bits and `s.pad_axes[]` from an XInput gamepad (does not touch `s.keys[]`).

- [ ] **Step 1: Write the failing mapping test.** Create `tests/test_xinput_map.cpp`:

```cpp
#include <doctest/doctest.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include "../harness/windowed/xinput_map.h"

TEST_CASE("xinput_to_pad maps buttons, sticks, and triggers to the abstract pad") {
    XINPUT_GAMEPAD gp{};
    gp.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_LEFT;
    gp.sThumbLX = 20000; gp.sThumbLY = -30000;
    gp.sThumbRX = 0;     gp.sThumbRY = 0;
    gp.bLeftTrigger = 255; gp.bRightTrigger = 0;

    rp_input_state s{};
    xinput_to_pad(gp, s);

    CHECK((s.pad_buttons & (1u << RP_PAD_A)));
    CHECK((s.pad_buttons & (1u << RP_PAD_START)));
    CHECK((s.pad_buttons & (1u << RP_PAD_DPAD_LEFT)));
    CHECK_FALSE((s.pad_buttons & (1u << RP_PAD_B)));
    CHECK(s.pad_axes[RP_AXIS_LEFT_X] == 20000);
    CHECK(s.pad_axes[RP_AXIS_LEFT_Y] == -30000);   // Y sign preserved (up = positive)
    CHECK(s.pad_axes[RP_AXIS_LEFT_TRIGGER] > 32000); // 255 -> near full
    CHECK(s.pad_axes[RP_AXIS_RIGHT_TRIGGER] == 0);
    CHECK(s.keys[0] == 0);                          // does not touch keys[]
}
```

- [ ] **Step 2: Create the pure mapping header.** Create `harness/windowed/xinput_map.h`:

```cpp
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include "retropark/retropark_abi.h"

// Pure mapping: XInput gamepad -> RetroPark abstract pad (pad_buttons bits + pad_axes[]). Does not
// touch keys[]. Sticks pass through (XInput is already -32768..32767, up = positive); triggers 0..255
// scale to 0..~32640. No XInput API calls here, so this is unit-testable without a controller.
inline void xinput_to_pad(const XINPUT_GAMEPAD& gp, rp_input_state& s) {
    auto set = [&](int bit, bool on) { if (on) s.pad_buttons |= (uint16_t)(1u << bit); };
    set(RP_PAD_A,          gp.wButtons & XINPUT_GAMEPAD_A);
    set(RP_PAD_B,          gp.wButtons & XINPUT_GAMEPAD_B);
    set(RP_PAD_X,          gp.wButtons & XINPUT_GAMEPAD_X);
    set(RP_PAD_Y,          gp.wButtons & XINPUT_GAMEPAD_Y);
    set(RP_PAD_L,          gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER);
    set(RP_PAD_R,          gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER);
    set(RP_PAD_SELECT,     gp.wButtons & XINPUT_GAMEPAD_BACK);
    set(RP_PAD_START,      gp.wButtons & XINPUT_GAMEPAD_START);
    set(RP_PAD_L3,         gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB);
    set(RP_PAD_R3,         gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB);
    set(RP_PAD_DPAD_UP,    gp.wButtons & XINPUT_GAMEPAD_DPAD_UP);
    set(RP_PAD_DPAD_DOWN,  gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
    set(RP_PAD_DPAD_LEFT,  gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    set(RP_PAD_DPAD_RIGHT, gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
    s.pad_axes[RP_AXIS_LEFT_X]        = gp.sThumbLX;
    s.pad_axes[RP_AXIS_LEFT_Y]        = gp.sThumbLY;
    s.pad_axes[RP_AXIS_RIGHT_X]       = gp.sThumbRX;
    s.pad_axes[RP_AXIS_RIGHT_Y]       = gp.sThumbRY;
    s.pad_axes[RP_AXIS_LEFT_TRIGGER]  = (int16_t)(gp.bLeftTrigger * 128);
    s.pad_axes[RP_AXIS_RIGHT_TRIGGER] = (int16_t)(gp.bRightTrigger * 128);
}
```

- [ ] **Step 3: Register the test.** In `tests/CMakeLists.txt`, add `test_xinput_map.cpp` to the `add_executable(retropark_tests …)` source list (after `test_dolphin_core_e2e.cpp`).

- [ ] **Step 4: Build + run the mapping test.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe --test-case="xinput*"`
Expected: PASS.

- [ ] **Step 5: Poll XInput in the harness.** In `harness/windowed/main.cpp`, add `#include "xinput_map.h"` with the other includes. In `read_local_input()` (currently fills `s.keys[]`), after the keyboard scan and before `return s;`, add:

```cpp
    XINPUT_STATE xi{};
    if (XInputGetState(0, &xi) == ERROR_SUCCESS)
        xinput_to_pad(xi.Gamepad, s);   // merge gamepad into the same rp_input_state
```

- [ ] **Step 6: Feed input in the single-player present path.** In `main.cpp`, in the final `else` branch of the main loop (the single-player path that calls `rp_runtime_present(g_rt, nullptr)`), feed input before presenting. Replace the branch body so both the rewind and normal present are preceded by an input update:

```cpp
        } else {
            rp_input_state local = read_local_input();
            rp_runtime_set_input(g_rt, 0, &local);   // host-owned input for driven AND presenting cores
            const bool rewinding = g_rewind_enabled && (GetAsyncKeyState(kRewindVK) & 0x8000) != 0;
            if (rewinding) {
                if (rp_runtime_rewind(g_rt) == RP_OK) {
                    rp_runtime_present(g_rt, nullptr);
                }
            } else {
                rp_runtime_present(g_rt, nullptr);   // normal forward present to the window
            }
        }
```

- [ ] **Step 7: Link xinput.** In `harness/windowed/CMakeLists.txt`, add `xinput` to the harness target's `target_link_libraries` (e.g. append `xinput` to the existing list). XInput ships in the Windows SDK; no package needed.

- [ ] **Step 8: Build everything; run the full suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: harness builds + links; suite `102 passed | 0 failed`.

- [ ] **Step 9: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add harness/windowed/xinput_map.h harness/windowed/main.cpp harness/windowed/CMakeLists.txt tests/test_xinput_map.cpp tests/CMakeLists.txt && git commit -m "feat(harness): XInput gamepad -> abstract pad + feed single-player input each present (keyboard + analog)"
```

---

### Task 3: Vehicle — inject host input into Dolphin's GC pad + gated proof (GREEN)

Seed port 0 as a GC controller and register a `SetInputOverrideFunction` that maps `rp_input_state` → GC controls; rebuild the DLL; add + pass the gated input-plumbing assertion.

**Files:**
- Modify: `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`
- Modify: `tests/test_dolphin_core_e2e.cpp` (gated input-plumbing assertion)
- Modify: `docs/patches/dolphin-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: `RP_PAD_*` / `RP_AXIS_*` (Task 1); `rp_runtime_input_poll_count` (Task 1); `g_producer.rp_host.input_state` (set in `dp_set_surfaces`, Slice K); Dolphin `Config::GetInfoForSIDevice(0)` + `SerialInterface::SIDEVICE_GC_CONTROLLER`; `Pad::IsInitialized()`, `Pad::GetConfig()->GetController(0)` (`ControllerEmu::EmulatedController*`) → `SetInputOverrideFunction`/`ClearInputOverrideFunction`.
- Produces: Dolphin's GC pad reflects host input; the gated test's `rp_runtime_input_poll_count > 0`.

- [ ] **Step 1: Write the failing gated assertion.** In `tests/test_dolphin_core_e2e.cpp`, feed a held input before the present loop and assert the poll counter after it. Right after `REQUIRE(rp_runtime_load_content(rt, kRom) == RP_OK);`, add:

```cpp
    // Slice M: host-owned input. Hold a strong input (full Control Stick left + A + Start); the vehicle's
    // override pulls it via rp_host.input_state each SI poll, which routes through Runtime::on_input.
    rp_input_state held{};
    held.pad_axes[RP_AXIS_LEFT_X] = -32767;                 // full left on the analog stick
    held.pad_buttons = (uint16_t)((1u << RP_PAD_A) | (1u << RP_PAD_START));
    rp_runtime_set_input(rt, 0, &held);
```

Then just before `rp_runtime_unload_core(rt);`, capture the count, and after the existing audio/overlay CHECKs add the assertion:

```cpp
    uint64_t input_polls = rp_runtime_input_poll_count(rt);
    fprintf(stderr, "[dolphin-core] input polls=%llu\n", (unsigned long long)input_polls); fflush(stderr);
```

(capture it next to the `rp_runtime_audio_stats` read, before unload), and with the other final CHECKs:

```cpp
    // Dolphin polled host input through the ABI (the override called rp_host.input_state each SI poll).
    CHECK(input_polls > 0);
```

- [ ] **Step 2: Build + run the gated test; verify the input assertion FAILS.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build && cmake --build . --config Debug --target retropark_tests && cd tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin core*"
```
Expected: FAILS on `CHECK(input_polls > 0)` — `[dolphin-core] input polls=0` (Dolphin has no override yet, so it never pulls host input). Audio/video still pass. (A WARN-skip means DLL/ROM/GPU missing — not the expected red.)

- [ ] **Step 3: Add input includes to the vehicle.** In `rp_dolphin.cpp`, after the existing audio includes, add:

```cpp
#include <optional>
#include <string_view>

#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/InputConfig.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
```

- [ ] **Step 4: Seed port 0 as a GC controller.** In `HostThread`, next to the audio-backend seed (`Config::SetBaseOrCurrent(Config::MAIN_AUDIO_BACKEND, …)`), add:

```cpp
  // Host-owned input (Slice M): port 0 is a standard GC controller so the SI polls it; our override
  // (installed after boot) feeds it from rp_host.input_state. Without this, nothing polls input.
  Config::SetBaseOrCurrent(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
```

- [ ] **Step 5: Install the input override after boot.** In `HostThread`, after the audio puller is started and before/at the top of the run loop, install the override once `Pad` is initialized. Add a latch and, inside the existing `while (g_running…)` loop body (top), add:

```cpp
    bool input_override_installed = false;
```

(declare just before the run loop), and as the first statements inside the loop:

```cpp
      if (!input_override_installed && Pad::IsInitialized() && Pad::GetConfig() &&
          Pad::GetConfig()->GetControllerCount() > 0)
      {
        Pad::GetConfig()->GetController(0)->SetInputOverrideFunction(
            [](std::string_view group, std::string_view control,
               ControlState /*state*/) -> std::optional<ControlState> {
              if (!g_producer.rp_host.input_state)
                return std::nullopt;   // direct C-API mode (Slice J): no host input, unchanged
              rp_input_state in{};
              g_producer.rp_host.input_state(g_producer.rp_host.host, 0, &in);
              auto key = [&](int vk) { return in.keys[vk] != 0; };
              auto btn = [&](int bit) { return ((in.pad_buttons >> bit) & 1u) != 0; };
              auto ax  = [&](int i) { return in.pad_axes[i] / 32767.0; };
              if (group == "Buttons") {
                if (control == "A")     return (btn(RP_PAD_A)     || key('K'))       ? 1.0 : 0.0;
                if (control == "B")     return (btn(RP_PAD_B)     || key('J'))       ? 1.0 : 0.0;
                if (control == "X")     return (btn(RP_PAD_X)     || key('L'))       ? 1.0 : 0.0;
                if (control == "Y")     return (btn(RP_PAD_Y)     || key('I'))       ? 1.0 : 0.0;
                if (control == "Z")     return (btn(RP_PAD_SELECT)|| key('U'))       ? 1.0 : 0.0;
                if (control == "Start") return (btn(RP_PAD_START) || key(VK_RETURN)) ? 1.0 : 0.0;
                return std::nullopt;
              }
              if (group == "Main Stick") {
                double x = ax(RP_AXIS_LEFT_X), y = ax(RP_AXIS_LEFT_Y);
                if (key(VK_LEFT))  x = -1.0;
                if (key(VK_RIGHT)) x =  1.0;
                if (key(VK_DOWN))  y = -1.0;
                if (key(VK_UP))    y =  1.0;
                if (control == "Up")    return y > 0 ?  y : 0.0;
                if (control == "Down")  return y < 0 ? -y : 0.0;
                if (control == "Left")  return x < 0 ? -x : 0.0;
                if (control == "Right") return x > 0 ?  x : 0.0;
                return std::nullopt;   // Modifier
              }
              if (group == "C-Stick") {
                double x = ax(RP_AXIS_RIGHT_X), y = ax(RP_AXIS_RIGHT_Y);
                if (control == "Up")    return y > 0 ?  y : 0.0;
                if (control == "Down")  return y < 0 ? -y : 0.0;
                if (control == "Left")  return x < 0 ? -x : 0.0;
                if (control == "Right") return x > 0 ?  x : 0.0;
                return std::nullopt;
              }
              if (group == "Triggers") {
                double lt = ax(RP_AXIS_LEFT_TRIGGER), rt = ax(RP_AXIS_RIGHT_TRIGGER);
                if (control == "L")        return (btn(RP_PAD_L) || lt > 0.5) ? 1.0 : 0.0;
                if (control == "R")        return (btn(RP_PAD_R) || rt > 0.5) ? 1.0 : 0.0;
                if (control == "L-Analog") return btn(RP_PAD_L) ? 1.0 : lt;
                if (control == "R-Analog") return btn(RP_PAD_R) ? 1.0 : rt;
                return std::nullopt;
              }
              if (group == "D-Pad") {
                if (control == "Up")    return btn(RP_PAD_DPAD_UP)    ? 1.0 : 0.0;
                if (control == "Down")  return btn(RP_PAD_DPAD_DOWN)  ? 1.0 : 0.0;
                if (control == "Left")  return btn(RP_PAD_DPAD_LEFT)  ? 1.0 : 0.0;
                if (control == "Right") return btn(RP_PAD_DPAD_RIGHT) ? 1.0 : 0.0;
                return std::nullopt;
              }
              return std::nullopt;
            });
        input_override_installed = true;
      }
```

- [ ] **Step 6: Clear the override on teardown.** In `HostThread`, after the run loop exits and after the audio thread join, before `Core::Stop(system)`, add:

```cpp
    if (input_override_installed && Pad::IsInitialized() && Pad::GetConfig() &&
        Pad::GetConfig()->GetControllerCount() > 0)
      Pad::GetConfig()->GetController(0)->ClearInputOverrideFunction();
```

- [ ] **Step 7: Rebuild `dolphin_present.dll` (PowerShell).**

```bash
powershell -NoProfile -Command '& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\Core\DolphinNoGUI\RetroParkDolphin.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:SolutionDir="C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\\" -p:BuildProjectReferences=false -m -v:minimal -nologo'
```
Expected: `dolphin_present.dll` relinks (only `rp_dolphin.cpp` recompiles). Confirm a fresh timestamp on `external/dolphin/Binary/x64/dolphin_present.dll`.

- [ ] **Step 8: Verify the DLL still exports the ABI.**

```bash
powershell -NoProfile -Command '$d=Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Select -First 1 -Expand FullName; & $d /exports "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Binary\x64\dolphin_present.dll" | Select-String "rp_get_core_abi"'
```
Expected: `rp_get_core_abi` listed; `external/dolphin/Binary/x64/core.json` present.

- [ ] **Step 9: Run the gated test; verify it now PASSES.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin core*"
```
Expected: `1 passed` — `[dolphin-core] input polls=<big>` (> 0), plus the audio/video/overlay CHECKs still green. (WARN-skip ⇒ DLL/ROM/GPU missing; resolve before claiming done.)

- [ ] **Step 10: Refresh the patch (stdout only).**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null; grep -c "SetInputOverrideFunction\|GetInfoForSIDevice\|Main Stick" docs/patches/dolphin-external-present.patch; grep -c "warning:" docs/patches/dolphin-external-present.patch
```
Expected: first count > 0 (input changes captured), second `0` (no CRLF contamination).

- [ ] **Step 11: Rebuild tests + run the FULL default suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `102 passed | 0 failed` (Dolphin e2e skips without the env var).

- [ ] **Step 12: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add tests/test_dolphin_core_e2e.cpp docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): Slice M — inject host input into Dolphin's GC pad via SetInputOverrideFunction (SI-device seed + keyboard/analog map)"
```

Note: `rp_dolphin.cpp` is under git-ignored `external/dolphin`, so `git add` of it is a no-op — the patch is the record.

---

## Intentionally not in this plan

- **The spec's best-effort "functional divergence" test** (held input makes the frame stream differ from
  neutral) is deliberately omitted: a real game's reaction is timing/determinism-dependent and would make
  a flaky gate. The reliable automated proof is the plumbing counter (Dolphin pulled host input through
  the ABI); the game visibly responding is the **harness human proof** (play Billy Hatcher with a pad or
  keyboard). If a rigorous automated effect-test is wanted later, it belongs in its own slice (likely
  needing Dolphin savestates for a single-boot A/B).

## Post-plan: verify + merge + memory

After Task 3 is green and reviewed: full suite green + the gated proof (input polls > 0, plus audio/video still pass), then merge to `main` + push `origin main` (no finish-branch menu, no AI attribution), then update the project memory (`retropark-project.md` + `MEMORY.md`) marking Slice M done, and note that single-player harness input was previously unwired (fixed here).
