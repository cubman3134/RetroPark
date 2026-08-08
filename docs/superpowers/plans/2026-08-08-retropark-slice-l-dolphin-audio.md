# RetroPark Slice L — Dolphin Audio Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route standalone Dolphin's 48 kHz stereo game audio through RetroPark's XAudio2 output (the Slice-E `audio_sample` path) so the host owns the final A/V mix; any GameCube ISO played through the `dolphin_present` core produces sound.

**Architecture:** Dolphin runs with the **Null** audio backend (no device); the DSP still feeds its `Mixer(48000)`. A puller thread in the vehicle (`rp_dolphin.cpp`) pulls `Mixer::Mix()` and forwards interleaved-stereo-s16 chunks to `rp_host.audio_sample`, which the Runtime already routes to `XAudio2Output` (Slice E). The presenting core reports its rate via the existing `get_av_info` hook so the Runtime opens audio. No ABI change.

**Tech Stack:** C++17, Dolphin (tag 2606) `AudioCommon` (`SoundStream`/`Mixer`), RetroPark `XAudio2Output`, doctest, MSBuild (Dolphin DLL) + CMake/MSBuild (RetroPark).

## Global Constraints

- **No AI attribution** in any commit message or PR body (no `Co-Authored-By`, no generated-by footer). Conventional prefixes (`feat:`/`fix:`/`docs:`) apply.
- **ABI stays v5** — no change to `include/retropark/retropark_abi.h`.
- **`external/dolphin` is git-ignored** — every Dolphin-side change is captured to `docs/patches/dolphin-external-present.patch` (regenerate with `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch` using **stdout only**, `2>/dev/null`, or the CRLF warnings contaminate the file).
- **Never commit cores or ROMs.** Billy Hatcher ROM: `C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz`.
- **Dolphin DLL build** (`RetroParkDolphin.vcxproj` → `dolphin_present.dll`): use the MSBuild recipe in Task 3 verbatim; Git Bash mangles `/`-style MSBuild switches (use `-` switches); use PowerShell for `dumpbin`. The DLL's `AfterBuild` copies `dolphin_present.dll` + `cores/dolphin_present/core.json` into `external/dolphin/Binary/x64/` (the rp_core dir the tests load).
- **RetroPark build:** `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`. Full suite: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`. The Dolphin e2e is opt-in via `RP_RUN_DOLPHIN=1` and WARN-skips without GPU/DLL/ROM.

---

### Task 1: Failing audio assertion in the gated Dolphin e2e (RED)

Add the audio-stats assertion to the existing Slice-K Runtime-path e2e so a single Dolphin boot proves video *and* audio. Written first, it fails (nothing opens/forwards audio yet).

**Files:**
- Modify: `tests/test_dolphin_core_e2e.cpp` (the `TEST_CASE` at line ~50; add audio assertions after the present loop, before/after the existing video checks)

**Interfaces:**
- Consumes (already public in `include/retropark/retropark.h`): `void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out);`
- Produces: nothing for later tasks (test-only).

- [ ] **Step 1: Add the audio-stats assertion.** In `tests/test_dolphin_core_e2e.cpp`, immediately **before** `rp_runtime_unload_core(rt);`, capture audio stats; and **after** the existing overlay `CHECK`, assert them. Insert before unload:

```cpp
    // Slice L: Dolphin's audio must reach the host through rp_host.audio_sample -> XAudio2 (device-
    // independent counters; they tally even with no output device). Read before teardown.
    uint64_t audio_frames = 0; int audio_nonsilent = 0;
    rp_runtime_audio_stats(rt, &audio_frames, &audio_nonsilent);
    fprintf(stderr, "[dolphin-core] audio: frames=%llu nonsilent=%d\n",
            (unsigned long long)audio_frames, audio_nonsilent); fflush(stderr);
```

Then, after the final overlay `CHECK(tint_tl > tint_br + 30.0);`, add:

```cpp
    // Dolphin's game audio reached RetroPark's output path, and it is real sound (not silence).
    CHECK(audio_frames > 0);
    CHECK(audio_nonsilent == 1);
```

- [ ] **Step 2: Build the tests.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug --target retropark_tests`
Expected: builds (benign C4996 warnings on getenv/fopen are pre-existing and OK).

- [ ] **Step 3: Run the gated e2e; verify the audio assertion FAILS.**

Run (from the test dir so relative paths resolve):
```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin core*"
```
Expected: the case runs (video still passes) but **FAILS** on `CHECK(audio_frames > 0)` — `[dolphin-core] audio: frames=0 nonsilent=0`. (If the DLL/ROM/GPU is absent it WARN-skips instead — the real proof is Task 3; do not treat a skip as a pass.)

- [ ] **Step 4: Commit the red test.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add tests/test_dolphin_core_e2e.cpp && git commit -m "test(dolphin): Slice L — assert Dolphin audio reaches the host (frames + non-silent), red"
```

---

### Task 2: Runtime opens audio for a presenting core that reports a rate

Teach `Runtime::load_content` to open host audio when a presenting content core reports a sample rate via `get_av_info`. Consumer half — keeps the full suite green (refcore_present_vk reports no rate → no audio, unchanged).

**Files:**
- Modify: `src/runtime/Runtime.cpp` (the `load_content` presenting branch, ~lines 197–214)

**Interfaces:**
- Consumes: `CoreLoader::get_av_info(rp_av_info* out, std::string& error)` → `RP_OK` and fills `out` if the core has `get_av_info`, else `RP_ERR_UNSUPPORTED`; `Runtime::open_audio(const rp_av_info& av)` (opens `XAudio2Output` at `av.sample_rate`, stereo; best-effort, no-op if `sample_rate <= 0`).
- Produces (for Task 3's producer): the Runtime queries `get_av_info` right after `start()` on a presenting content core; the core must return `sample_rate = 48000` there for audio to open.

- [ ] **Step 1: Add the open-audio call.** In `src/runtime/Runtime.cpp`, in the `if (core_type_ == RP_CORE_PRESENTING)` branch of `load_content`, after the block that starts the core (`if (loader_.state() != LoaderState::Started) { ... }`) and before `return RP_OK;`, insert:

```cpp
        // A presenting core that produces audio reports its rate via get_av_info (e.g. dolphin_present
        // pulls Dolphin's 48 kHz mix and forwards it through audio_sample). Open host audio best-effort;
        // a presenting core without get_av_info (refcore_present_vk) returns UNSUPPORTED and stays silent.
        rp_av_info av{};
        if (loader_.get_av_info(&av, err) == RP_OK && av.sample_rate > 0.0)
            open_audio(av);
```

- [ ] **Step 2: Build RetroPark.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`
Expected: builds clean.

- [ ] **Step 3: Run the full suite; verify no regression.**

Run: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `100 passed | 0 failed` (the gated Dolphin e2e WARN-skips without `RP_RUN_DOLPHIN`; refcore_present_vk opens no audio and its vk e2e still passes).

- [ ] **Step 4: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add src/runtime/Runtime.cpp && git commit -m "feat(runtime): open host audio for a presenting core that reports a sample rate via get_av_info"
```

---

### Task 3: Dolphin producer — Null backend, rate restore, puller thread, dp_get_av_info (GREEN)

Make the vehicle forward Dolphin's mix. All in `rp_dolphin.cpp`; rebuild the DLL; refresh the patch; then the gated e2e goes green.

**Files:**
- Modify: `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`
- Modify: `docs/patches/dolphin-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: `Core::System::GetInstance().GetSoundStream()` → `SoundStream*`; `SoundStream::GetMixer()` → `Mixer*`; `Mixer::SetSampleRate(u32)`, `Mixer::Mix(s16* out, size_t frames)` → frames written (interleaved stereo); `g_producer.rp_host` (`rp_host_iface` with `audio_sample`, set in `dp_set_surfaces`); file-scope `std::atomic<bool> g_running`; `Config::MAIN_AUDIO_BACKEND`, `BACKEND_NULLSOUND` (both from the already-included `Core/Config/MainSettings.h`).
- Produces: `dp_get_av_info` reporting `sample_rate = 48000` (consumed by Task 2's Runtime query); the puller calling `rp_host.audio_sample` (consumed by the Runtime → satisfies Task 1's assertion).

- [ ] **Step 1: Add audio includes.** In `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`, after the existing `#include "VideoBackends/Vulkan/VulkanContext.h"` (line ~38), add:

```cpp
#include <vector>

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
```

(`<vector>` may already be transitively present; an explicit include is harmless. `MAIN_AUDIO_BACKEND`/`BACKEND_NULLSOUND` need no new include — `Core/Config/MainSettings.h` is already included.)

- [ ] **Step 2: Seed the Null audio backend.** In `HostThread`, right after the existing `Config::SetBaseOrCurrent(Config::GFX_BACKEND_MULTITHREADING, false);` (line ~326), add:

```cpp
  // Host-owned audio (Slice L): Dolphin opens NO device — the DSP still feeds the SoundStream's Mixer,
  // and our puller thread (below) pulls that mix and forwards it to the host via rp_host.audio_sample.
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_BACKEND, BACKEND_NULLSOUND);
```

- [ ] **Step 3: Start the puller after boot; stop it before teardown.** In `HostThread`, inside `if (boot && BootManager::BootCore(system, std::move(boot), wsi))`, replace the run loop + shutdown region (currently:)

```cpp
    while (g_running.load(std::memory_order_acquire))
    {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Core::HostDispatchJobs(system);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Core::Stop(system);
    Core::Shutdown(system);
```

with:

```cpp
    // Audio puller (Slice L): NullSound::Init() zeroes the mixer output rate, so latch it back to
    // 48 kHz on the first valid SoundStream, then pull ~10 ms chunks of interleaved stereo s16 and
    // forward them to the host. Runs only in rp_core mode (rp_host.audio_sample set by dp_set_surfaces);
    // the Slice-J direct C-API path leaves it null, so audio stays silent there — unchanged. Pacing is
    // naive wall-clock (A/V sync is a deferred slice); XAudio2's queue absorbs the jitter.
    std::thread audio_thread([&system]() {
      constexpr std::size_t kFrames = 480;  // ~10 ms @ 48 kHz
      std::vector<int16_t> buf(kFrames * 2, 0);
      bool rate_set = false;
      while (g_running.load(std::memory_order_acquire))
      {
        SoundStream* ss = system.GetSoundStream();
        if (ss && g_producer.rp_host.audio_sample)
        {
          if (!rate_set) { ss->GetMixer()->SetSampleRate(48000); rate_set = true; }
          std::size_t got = ss->GetMixer()->Mix(buf.data(), kFrames);
          if (got > 0)
            g_producer.rp_host.audio_sample(g_producer.rp_host.host, buf.data(), got);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });

    while (g_running.load(std::memory_order_acquire))
    {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Core::HostDispatchJobs(system);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Stop pulling BEFORE Core::Shutdown destroys the SoundStream/Mixer.
    if (audio_thread.joinable())
      audio_thread.join();
    Core::Stop(system);
    Core::Shutdown(system);
```

- [ ] **Step 4: Wire dp_get_av_info into the vtable.** Add the function in the Slice-K anonymous namespace (near `dp_get_info`, before `const rp_core_abi kAbi`):

```cpp
void dp_get_av_info(rp_core*, rp_av_info* out)
{
  // Dolphin's SoundStream mixer runs at a fixed 48 kHz (we force it there after the Null backend zeroes
  // it); the producer pulls that mix and forwards interleaved stereo s16, so the host opens XAudio2 at
  // this rate. Geometry is unused (presenting core, not driven-validated). Constant to avoid racing the
  // async boot — the Runtime queries this right after start(), before the SoundStream may exist.
  out->fps = 0.0;
  out->sample_rate = 48000.0;
  out->base_width = 0; out->base_height = 0; out->max_width = 0; out->max_height = 0;
  out->pixel_format = 0;
}
```

Then in `const rp_core_abi kAbi = {...}`, set the `get_av_info` slot (the **first** `nullptr` — position 7, right after `dp_stop`) to `dp_get_av_info`:

```cpp
const rp_core_abi kAbi = {
    RETROPARK_ABI_VERSION, dp_get_info,    dp_create,      dp_destroy, dp_set_surfaces,
    dp_start,              dp_stop,        dp_get_av_info, nullptr,    nullptr,
    nullptr,               nullptr,        dp_load_content};
```

- [ ] **Step 5: Rebuild `dolphin_present.dll` (PowerShell).**

```bash
powershell -NoProfile -Command '& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\Core\DolphinNoGUI\RetroParkDolphin.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:SolutionDir="C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\\" -m -v:minimal -nologo'
```
Expected: `dolphin_present.dll` builds. It only recompiles `rp_dolphin.cpp` + links. **If the glslang external project errors** on a `mkdir -p`/`copy` custom-command (a known spurious re-config flake, unrelated to our DLL): the DLL itself still builds — confirm by checking the timestamp (Step 6). If the DLL did not relink, retry the same command once; the flake is in a sibling externals target.

- [ ] **Step 6: Verify the DLL rebuilt and exports the audio-carrying ABI.**

```bash
powershell -NoProfile -Command '$d=Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Select -First 1 -Expand FullName; & $d /exports "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Binary\x64\dolphin_present.dll" | Select-String "rp_get_core_abi"'
```
Expected: `rp_get_core_abi` listed. Also confirm `external/dolphin/Binary/x64/dolphin_present.dll` mtime is fresh and `external/dolphin/Binary/x64/core.json` exists (AfterBuild copies it).

- [ ] **Step 7: Run the gated e2e; verify it now PASSES (audio green).**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin core*"
```
Expected: `1 passed` — `[dolphin-core] audio: frames=<big> nonsilent=1`, and the video/overlay checks still pass. (If it WARN-skips, the DLL/ROM/GPU is missing — resolve before claiming done.)

- [ ] **Step 8: Refresh the Dolphin patch (stdout only — no CRLF contamination).**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/dolphin add -N Source/Core/DolphinNoGUI/rp_dolphin.cpp 2>/dev/null; git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null; grep -c "audio_sample\|MAIN_AUDIO_BACKEND\|dp_get_av_info" docs/patches/dolphin-external-present.patch; grep -c "warning:" docs/patches/dolphin-external-present.patch
```
Expected: the first count > 0 (audio changes captured), the second `0` (no warning contamination).

- [ ] **Step 9: Rebuild the RetroPark tests and run the FULL suite (no regression).**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: `100 passed | 0 failed` (default run; the Dolphin e2e skips without the env var).

- [ ] **Step 10: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): Slice L — forward Dolphin's 48kHz mix to the host (Null backend + mixer-pull thread + dp_get_av_info)"
```

Note: `rp_dolphin.cpp` lives under git-ignored `external/dolphin`, so `git add` of it is a no-op (the patch is the record) — that's expected; the commit carries the patch + any RetroPark-side files.

---

## Post-plan: merge + memory

After Task 3 is green and reviewed, per standing RetroPark practice: verify the full suite, merge to `main`, push `origin main` (no finish-branch menu, no AI attribution), then update the project memory (`retropark-project.md` + `MEMORY.md`) marking Slice L done.
