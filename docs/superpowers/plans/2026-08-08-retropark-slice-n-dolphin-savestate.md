# RetroPark Slice N — Dolphin Savestates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `dolphin_present`'s serialize hooks to Dolphin's native in-memory savestate so `rp_runtime_save_state`/`load_state` capture and restore a running GameCube game through the core ABI.

**Architecture:** Expose Dolphin's existing (static) `SaveToBuffer`/`LoadFromBuffer` as public wrappers that run on the CPU thread via `Core::RunOnCPUThread` and wait for completion; the vehicle's `dp_serialize_size`/`serialize`/`unserialize` drive them and fill the `kAbi` serialize slots. RetroPark's Runtime is unchanged (`save_state`/`load_state` already work for any serialize-capable core). Proven by a gated round-trip determinism e2e.

**Tech Stack:** C++17, Dolphin (tag 2606) `Core::State`/`RunOnCPUThread`/`Common::UniqueBuffer`, RetroPark Runtime ABI serialize hooks, doctest, MSBuild (Dolphin DLL) + CMake/MSBuild (RetroPark).

## Global Constraints

- **No AI attribution** in any commit message or PR body. Conventional prefixes (`feat:`/`fix:`/`docs:`).
- **ABI stays v5** — do NOT change `RETROPARK_ABI_VERSION` or the `rp_core_abi` struct. The serialize hooks already exist; this slice only fills `dolphin_present`'s null serialize slots.
- **`external/dolphin` is git-ignored** — Dolphin-side changes are captured to `docs/patches/dolphin-external-present.patch`. Regenerate stdout-only: `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null` (stderr CRLF warnings corrupt the file otherwise).
- **Never commit cores or ROMs.** Billy Hatcher ROM: `C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz`.
- **Dolphin DLL relink recipe** (Task 2): use `-p:BuildProjectReferences=false` (recompiles only the touched files + relinks; a plain rebuild hard-fails at the sibling `glslang` project's VS17/VS18 cmake-cache mismatch). Task 2 edits **`State.cpp`, which is compiled by `DolphinLib.vcxproj`** (`external/dolphin/Source/Core/DolphinLib.vcxproj`, via `DolphinLib.props`) — a static lib the DLL links. With `BuildProjectReferences=false` the `RetroParkDolphin` project won't rebuild DolphinLib, so **build `DolphinLib.vcxproj` FIRST** (same flags) to recompile `State.cpp` into `DolphinLib.lib`, THEN build `RetroParkDolphin.vcxproj` to recompile `rp_dolphin.cpp` and relink `dolphin_present.dll` against the updated lib. All via **PowerShell** (Git Bash mangles `/`-switches); use PowerShell for `dumpbin` too. AfterBuild copies `dolphin_present.dll` + `core.json` into `external/dolphin/Binary/x64/`.
- **RetroPark build:** `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`. Full suite: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe` → `102 passed | 0 failed`. The Dolphin e2e tests are opt-in `RP_RUN_DOLPHIN=1`, WARN-skip without GPU/DLL/ROM.

---

### Task 1: Failing gated round-trip savestate e2e (RED)

Write the gated determinism test first. It fails now because `dolphin_present` has null serialize hooks, so `rp_runtime_serialize_size` returns 0 and `save_state` returns `RP_ERR_UNSUPPORTED`.

**Files:**
- Create: `tests/test_dolphin_savestate_e2e.cpp`
- Modify: `tests/CMakeLists.txt` (register the new test)

**Interfaces:**
- Consumes (all existing public API in `include/retropark/retropark.h`): `rp_runtime_create`, `rp_runtime_resize`, `rp_runtime_load_core`, `rp_runtime_load_content`, `rp_runtime_present`, `rp_runtime_set_input`, `rp_runtime_serialize_size`, `rp_runtime_save_state`, `rp_runtime_load_state`, `rp_runtime_unload_core`, `rp_runtime_destroy`.
- Produces: nothing (test-only).

- [ ] **Step 1: Write the test.** Create `tests/test_dolphin_savestate_e2e.cpp`:

```cpp
#include <doctest/doctest.h>
#include <retropark/retropark.h>
#include "render/vulkan/VulkanBackend.h"
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rp;

// Slice N: Dolphin savestate round-trip through the core ABI. Boot Billy Hatcher, advance to a settled
// frame, save_state, advance K frames (capture A), load_state (restore), advance K again (capture B),
// and assert B == A: the restored state re-runs deterministically to the same frame. Input is held
// neutral across both advances so the re-simulation is deterministic. Device-independent readback.

#ifndef RP_DOLPHIN_CORE_DIR
#define RP_DOLPHIN_CORE_DIR "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64"
#endif

namespace {
const char* kCoreDir = RP_DOLPHIN_CORE_DIR;
const char* kRom = "C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz";
bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Present until `count` good frames have been consumed (Dolphin boots over several seconds; present()
// returns non-OK until frames flow). Returns the number of good presents actually achieved.
int pump(rp_runtime* rt, std::vector<uint8_t>& img, int count) {
    int good = 0;
    for (int i = 0; i < count * 6 + 600 && good < count; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (rp_runtime_present(rt, img.data()) == RP_OK) ++good;
    }
    return good;
}
size_t bytes_differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t d = 0; for (size_t i = 0; i < a.size() && i < b.size(); ++i) if (a[i] != b[i]) ++d; return d;
}
} // namespace

TEST_CASE("dolphin savestate: save -> diverge -> load re-runs deterministically to the same frame (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping Dolphin savestate e2e"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/dolphin_present.dll")) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists(std::string(kCoreDir) + "/core.json")) { WARN("core.json not beside the DLL; skipping"); return; }
    if (!file_exists(kRom)) { WARN("Billy Hatcher ROM absent; skipping"); return; }

    const uint32_t W = 640, H = 480;
    rp_runtime* rt = rp_runtime_create(RP_GFX_VULKAN, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, W, H) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, kCoreDir) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, kRom) == RP_OK);

    rp_input_state neutral{};
    rp_runtime_set_input(rt, 0, &neutral);   // hold neutral so the re-simulation is deterministic

    std::vector<uint8_t> img(W * H * 4, 0), frameN, A, B;
    const int K = 60;

    // Advance to a settled frame N (~5s of boot), capture it.
    REQUIRE(pump(rt, img, 260) >= 260);
    frameN = img;

    // Save state here.
    size_t sz = rp_runtime_serialize_size(rt);
    fprintf(stderr, "[dolphin-save] serialize_size=%zu\n", sz); fflush(stderr);
    REQUIRE(sz > 0);                                  // FAILS until dolphin_present wires serialize
    std::vector<uint8_t> state(sz, 0);
    REQUIRE(rp_runtime_save_state(rt, state.data(), state.size()) == RP_OK);

    // Advance K frames -> A.
    REQUIRE(pump(rt, img, K) >= K);
    A = img;
    CHECK(bytes_differing(A, frameN) > A.size() / 50);   // game actually advanced (not frozen)

    // Restore, advance K again -> B.
    REQUIRE(rp_runtime_load_state(rt, state.data(), state.size()) == RP_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let the post-load pipeline settle
    REQUIRE(pump(rt, img, K) >= K);
    B = img;

    size_t diff = bytes_differing(A, B);
    fprintf(stderr, "[dolphin-save] A-vs-B differing bytes=%zu of %zu\n", diff, A.size()); fflush(stderr);

    rp_runtime_unload_core(rt);
    rp_runtime_destroy(rt);

    // The restored state re-ran to the same frame. Byte-identical is the target; a tight tolerance
    // (< 0.1% of bytes) absorbs any minor GPU-path nondeterminism without weakening the claim.
    CHECK(diff <= A.size() / 1000);
}
```

- [ ] **Step 2: Register the test.** In `tests/CMakeLists.txt`, add `test_dolphin_savestate_e2e.cpp` to the `add_executable(retropark_tests …)` source list (after `test_xinput_map.cpp`).

- [ ] **Step 3: Build the tests.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests`
Expected: builds (benign C4996 getenv/fopen warnings OK).

- [ ] **Step 4: Run the gated test; verify it FAILS red.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin savestate*"
```
Expected: FAILS — `[dolphin-save] serialize_size=0` then `REQUIRE(sz > 0)` fails (dolphin_present has no serialize hooks yet). (A WARN-skip means DLL/ROM/GPU missing — not the expected red.)

- [ ] **Step 5: Commit the red test.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add tests/test_dolphin_savestate_e2e.cpp tests/CMakeLists.txt && git commit -m "test(dolphin): Slice N — gated savestate round-trip determinism e2e (red)"
```

---

### Task 2: Dolphin in-memory savestate + vehicle serialize hooks (GREEN)

Expose Dolphin's buffer savestate (CPU-thread-synced) and wire the vehicle's serialize hooks; rebuild the DLL; green the round-trip test.

**Files:**
- Modify: `external/dolphin/Source/Core/Core/State.h` (declare the two public wrappers)
- Modify: `external/dolphin/Source/Core/Core/State.cpp` (define them)
- Modify: `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp` (serialize hooks + kAbi)
- Modify: `docs/patches/dolphin-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: Dolphin `Core::RunOnCPUThread(Core::System&, Common::MoveOnlyFunction<void()>)`; the existing file-static `SaveToBuffer(Core::System&, Common::UniqueBuffer<u8>&) -> std::size_t` and `LoadFromBuffer(Core::System&, std::span<u8>) -> bool` in `State.cpp`; `Core::System::GetInstance()`; the vehicle's `g_running` atomic.
- Produces: `std::size_t State::SaveToBufferOnCPUThread(Core::System&, Common::UniqueBuffer<u8>&)` and `bool State::LoadFromBufferOnCPUThread(Core::System&, std::span<u8>)`; the `dp_serialize_size`/`dp_serialize`/`dp_unserialize` core functions wired into `kAbi`.

- [ ] **Step 1: Declare the public wrappers.** In `external/dolphin/Source/Core/Core/State.h`, inside `namespace State`, near the existing `Save`/`Load` declarations, add (ensure `#include <span>` and `#include "Common/Buffer.h"` are present at the top of the header — add them if missing):

```cpp
// RetroPark: synchronous in-memory save/load, run on the CPU thread and waited on (so the caller gets a
// fully-captured buffer). Returns the state size (0 on failure) / success.
std::size_t SaveToBufferOnCPUThread(Core::System& system, Common::UniqueBuffer<u8>& buffer);
bool LoadFromBufferOnCPUThread(Core::System& system, std::span<u8> buffer);
```

- [ ] **Step 2: Define the wrappers.** In `external/dolphin/Source/Core/Core/State.cpp`, ensure `#include <future>` is present near the top (add it if missing), then add these definitions inside `namespace State` (place them just after the file-static `SaveToBuffer`/`LoadFromBuffer` definitions so those names are in scope):

```cpp
std::size_t SaveToBufferOnCPUThread(Core::System& system, Common::UniqueBuffer<u8>& buffer)
{
  std::size_t result = 0;
  std::promise<void> done;
  // RunOnCPUThread is fire-and-forget when off the CPU thread; the promise makes us wait for the save.
  Core::RunOnCPUThread(system, [&] {
    result = SaveToBuffer(system, buffer);
    done.set_value();
  });
  done.get_future().wait();
  return result;
}

bool LoadFromBufferOnCPUThread(Core::System& system, std::span<u8> buffer)
{
  bool ok = false;
  std::promise<void> done;
  Core::RunOnCPUThread(system, [&] {
    ok = LoadFromBuffer(system, buffer);
    done.set_value();
  });
  done.get_future().wait();
  return ok;
}
```

- [ ] **Step 3: Add the vehicle serialize hooks.** In `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`, add the include near the others:

```cpp
#include "Core/State.h"
#include "Common/Buffer.h"
```

Then, in the Slice-K anonymous namespace (near `dp_load_content`, before `const rp_core_abi kAbi`), add the state buffer + three functions:

```cpp
Common::UniqueBuffer<u8> g_state_buf;   // last captured savestate
size_t g_state_size = 0;                // its actual byte length (buffer may be larger)

size_t dp_serialize_size(rp_core*)
{
  if (!g_running.load(std::memory_order_acquire))
    return 0;   // not booted -> Runtime::save_state reports RP_ERR_UNSUPPORTED
  g_state_size = State::SaveToBufferOnCPUThread(Core::System::GetInstance(), g_state_buf);
  return g_state_size;
}
rp_result dp_serialize(rp_core*, void* data, size_t size)
{
  if (!data || g_state_size == 0 || size < g_state_size)
    return RP_ERR_BAD_ARG;
  std::memcpy(data, g_state_buf.data(), g_state_size);   // snapshot captured at the serialize_size call
  return RP_OK;
}
rp_result dp_unserialize(rp_core*, const void* data, size_t size)
{
  if (!g_running.load(std::memory_order_acquire) || !data || size == 0)
    return RP_ERR_BAD_ARG;
  std::vector<u8> tmp(size);
  std::memcpy(tmp.data(), data, size);
  return State::LoadFromBufferOnCPUThread(Core::System::GetInstance(), std::span<u8>(tmp.data(), tmp.size()))
             ? RP_OK : RP_ERR_INTERNAL;
}
```

- [ ] **Step 4: Wire them into the vtable.** In `const rp_core_abi kAbi = {…}`, fill the three null serialize slots (positions after `run_frame`: `serialize_size`, `serialize`, `unserialize`). The vtable field order is: `…, get_av_info, run_frame, serialize_size, serialize, unserialize, load_content`. Change:

```cpp
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, dp_get_info,    dp_create,        dp_destroy,   dp_set_surfaces,
    dp_start,              dp_stop,        dp_get_av_info,   nullptr,      dp_serialize_size,
    dp_serialize,          dp_unserialize, dp_load_content};
```

(`run_frame` stays `nullptr` — presenting core. The three formerly-null slots now hold the serialize fns.)

- [ ] **Step 5: Recompile DolphinLib (for State.cpp) then relink the DLL (PowerShell).**

```bash
powershell -NoProfile -Command '$ms="C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe"; $sln="C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\\"; & $ms "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\Core\DolphinLib.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=$sln -p:BuildProjectReferences=false -m -v:minimal -nologo; & $ms "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\Core\DolphinNoGUI\RetroParkDolphin.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:SolutionDir=$sln -p:BuildProjectReferences=false -m -v:minimal -nologo'
```

Expected: `DolphinLib.vcxproj` recompiles the edited `State.cpp` into `DolphinLib.lib`, then `RetroParkDolphin.vcxproj` recompiles `rp_dolphin.cpp` and relinks `dolphin_present.dll`. If `State.h`/`State.cpp` report compile errors (missing include, name clash with the file-static `SaveToBuffer`), fix them and rebuild. A link error `unresolved external SaveToBufferOnCPUThread` means DolphinLib didn't recompile — rebuild `DolphinLib.vcxproj` (confirm it reports `State.cpp` compiling) before relinking. If the sibling `glslang` externals project errors on its `mkdir -p`/`copy` custom command, that's the known spurious flake — the DLL still relinks; confirm via a fresh DLL timestamp (Step 6).

- [ ] **Step 6: Verify the DLL relinked and still exports the ABI.**

```bash
powershell -NoProfile -Command '$d=Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Select -First 1 -Expand FullName; & $d /exports "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Binary\x64\dolphin_present.dll" | Select-String "rp_get_core_abi"'
```
Expected: `rp_get_core_abi` listed; `external/dolphin/Binary/x64/dolphin_present.dll` mtime fresh (newer than your edits); `core.json` present.

- [ ] **Step 7: Run the gated round-trip test; verify it now PASSES.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin savestate*"
```
Expected: `1 passed` — `[dolphin-save] serialize_size=<big>` (> 0, typically several MB) and `A-vs-B differing bytes` within tolerance (ideally 0). If A-vs-B exceeds the tolerance, the post-load pipeline likely needs more settle: increase the `sleep_for` after `load_state` (e.g. 300 ms) and/or drain a few presents before the K-count; re-run. If it stays high after reasonable settling, report it as a concern with the observed diff — do NOT loosen the tolerance beyond the spec's 0.1% without flagging.

- [ ] **Step 8: Refresh the Dolphin patch (stdout only).**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/dolphin add -N Source/Core/DolphinNoGUI/rp_dolphin.cpp 2>/dev/null; git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null; grep -c "OnCPUThread\|dp_serialize\|SaveToBuffer" docs/patches/dolphin-external-present.patch; grep -c "warning:" docs/patches/dolphin-external-present.patch
```
Expected: first count > 0 (savestate changes captured), second `0` (no CRLF contamination).

- [ ] **Step 9: Rebuild RetroPark tests + run the FULL default suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `103 passed | 0 failed` (102 prior + the new savestate case, which WARN-skips without the env var — so actually the case count is 103 but it skips; the suite reports `103 passed` with the gated one skipped or passed-as-skip. Confirm 0 failed and the driven savestate tests still pass.)

- [ ] **Step 10: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): Slice N — in-memory savestate through the ABI (expose SaveToBuffer/LoadFromBuffer CPU-thread-synced + wire dp_serialize hooks)"
```

Note: `rp_dolphin.cpp`, `State.h`, `State.cpp` are under git-ignored `external/dolphin` — `git add` of them is a no-op; the patch is the record.

---

### Task 3: Harness `--core` flag so Dolphin can be run + F5/F7-saved by hand

The harness can only load the built-in refcores; add a `--core <dir>` flag so it can load `dolphin_present` (with `--content <iso>`). The F5/F7 handler is already generic, so save/load then work for Dolphin — and this lets the user hand-verify Dolphin play/input/audio too.

**Files:**
- Modify: `harness/windowed/main.cpp` (parse `--core <dir>`; use it to load the core)

**Interfaces:**
- Consumes: `rp_runtime_load_core`, `rp_runtime_load_content`, `rp_runtime_resize` (existing). The F5/F7 handler and its `rp_runtime_serialize_size`/`save_state`/`load_state` calls already exist and are generic.
- Produces: nothing for later tasks (harness-only).

- [ ] **Step 1: Parse the flag.** In `harness/windowed/main.cpp`, in the `argv` parse loop (alongside `--content`), add a branch. Find the block that handles `else if (a == L"--content" && i + 1 < argc)` and add before/after it:

```cpp
                } else if (a == L"--core" && i + 1 < argc) {
                    custom_core_dir = narrow(argv[i + 1]);
```

Declare `std::string custom_core_dir;` next to the existing `content_path` declaration.

- [ ] **Step 2: Use the flag to load the core.** Replace the core-load block (the `if (use_content) { … } else { … }` around `rp_runtime_load_core`) so a custom core dir takes precedence:

```cpp
    if (!custom_core_dir.empty()) {
        // Arbitrary Vulkan presenting core (e.g. dolphin_present). --content feeds it the ROM; F5/F7 then
        // save/load it via the already-generic key handler.
        rp_runtime_resize(g_rt, 640, 480);
        rp_runtime_load_core(g_rt, custom_core_dir.c_str());
        if (use_content) rp_runtime_load_content(g_rt, content_path.c_str());
    } else if (use_content) {
        rp_runtime_resize(g_rt, 256, 240);   // NES resolution
        rp_runtime_load_core(g_rt, core_dir);
        rp_runtime_load_content(g_rt, content_path.c_str());
    } else {
        rp_runtime_resize(g_rt, 640, 480);
        rp_runtime_load_core(g_rt, core_dir);
    }
```

(Leave the existing `core_dir`/`core_id` selection above it untouched; `custom_core_dir` just overrides the load path when set.)

- [ ] **Step 3: Build the harness + full suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `retropark_harness.exe` builds/links; suite `103 passed | 0 failed`.

- [ ] **Step 4: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add harness/windowed/main.cpp && git commit -m "feat(harness): --core <dir> flag to load an arbitrary presenting core (run + F5/F7-save Dolphin by hand)"
```

---

## Post-plan: verify + merge + memory

After Task 2 is green and reviewed: full suite green + the gated round-trip proof (serialize_size > 0, A == B within tolerance), then merge to `main` + push `origin main` (no finish-branch menu, no AI attribution), then update the project memory (`retropark-project.md` + `MEMORY.md`) marking Slice N done — noting Dolphin savestate is the foundation for Dolphin rewind/netplay, and that cross-build/session portability is out of scope.
