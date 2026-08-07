# RetroPark Slice E (Audio Output) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Play a driven libretro core's audio (NES sound) through an OS audio device — wire the already-declared `audio_sample` callback to a new XAudio2-backed `IAudioOutput`, with no core-ABI change.

**Architecture:** A new `IAudioOutput` interface + `XAudio2Output` (a source voice that resamples internally, fed from a mutex-guarded pool of buffers recycled via `OnBufferEnd`, drop-on-full). The runtime opens audio when a driven core reports `sample_rate > 0`, forwards `audio_sample` → `submit` (counting frames + non-silence for a device-independent test), and closes on unload — all best-effort (no device = silent game, never fatal). The libretro shim stops dropping audio and forwards it.

**Tech Stack:** C++17, CMake, XAudio2 (Windows SDK, `xaudio2.lib`), the existing driven/libretro pipeline, doctest. Real validation: FCEUmm + a NES ROM (present on this machine; git-ignored).

## Global Constraints

- **C++17. No Qt/EverythingBox.** MSVC `/W4 /permissive-`, warning-clean.
- **No core-ABI change.** `RETROPARK_ABI_VERSION` stays `4`; `rp_core_abi`/`rp_host_iface` are unchanged (the `audio_sample` host callback already exists). The only additive surface is one diagnostic C API `rp_runtime_audio_stats` in `retropark.h`.
- **Audio is best-effort:** `XAudio2Output::open` failure (no device / init fail) is logged and non-fatal — `load_core` still succeeds and the game runs silent. `on_audio_sample` counts frames + non-silence regardless of whether a device opened (so the flow test is device-independent).
- **Format:** stereo (`channels = 2`), interleaved int16, rate from `rp_av_info.sample_rate`. Pool is bounded; if no free buffer → drop the batch (no block/crash). Pool access is mutex-guarded (present thread submits; XAudio2 thread recycles via `OnBufferEnd`).
- **The whole A–D suite stays green** (audio is additive; presenting/driven/libretro paths unchanged except the shim now forwards audio, covered by the flow e2e).
- **Gated real-core tests** (FCEUmm+ROM) and the **device smoke** `WARN`-skip when their prerequisite is absent (core/ROM, or an audio device), like the existing probe-guarded tests. Never commit FCEUmm/ROMs.
- **Vulkan** paths stay validation-clean; `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0` before any fresh `cmake -S . -B build`.
- **Commits:** conventional prefixes. **No AI attribution** anywhere.

---

## File Structure

```
src/audio/
  IAudioOutput.h                     # interface                                             (Task 1)
  AudioPool.h/.cpp                   # audio_pick_free_slot (pure, tested)                   (Task 1)
  XAudio2Output.h/.cpp               # XAudio2 impl + pooled buffers + OnBufferEnd recycle    (Task 2)
include/retropark/retropark.h        # + rp_runtime_audio_stats (additive diagnostic)        (Task 3)
src/runtime/Runtime.h/.cpp           # audio_ output, on_audio_sample forward+count, open/close (Task 3)
cores/libretro_shim/LibretroShim.cpp # forward audio callbacks; GET_AUDIO_VIDEO_ENABLE audio-on (Task 4)
tests/
  test_audio_pool.cpp                # pick_free_slot / drop-on-full (pure)                  (Task 1)
  test_audio_flow.cpp                # gated audio-flow e2e (device-independent) + device smoke (Task 5)
```

---

## Task 1: `IAudioOutput` interface + pure pool helper

**Files:**
- Create: `src/audio/IAudioOutput.h`, `src/audio/AudioPool.h`, `src/audio/AudioPool.cpp`, `tests/test_audio_pool.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct rp::IAudioOutput { virtual ~IAudioOutput(); virtual rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err)=0; virtual void submit(const int16_t* frames, size_t num_frames)=0; virtual void close()=0; };`
  - `int rp::audio_pick_free_slot(const bool* in_use, uint32_t n);` — returns the first index where `!in_use[i]`, or `-1` if all are in use.

- [ ] **Step 1: Write the failing test**

`tests/test_audio_pool.cpp`:
```cpp
#include <doctest/doctest.h>
#include "audio/AudioPool.h"
using rp::audio_pick_free_slot;

TEST_CASE("audio pool: picks first free slot") {
    bool s[4] = {true, true, false, true};
    CHECK(audio_pick_free_slot(s, 4) == 2);
}
TEST_CASE("audio pool: all in use -> -1 (drop-on-full)") {
    bool s[3] = {true, true, true};
    CHECK(audio_pick_free_slot(s, 3) == -1);
}
TEST_CASE("audio pool: first slot free") {
    bool s[3] = {false, true, true};
    CHECK(audio_pick_free_slot(s, 3) == 0);
}
```

- [ ] **Step 2: Write the interface + pool header**

`src/audio/IAudioOutput.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <retropark/retropark_abi.h>
namespace rp {
struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) = 0;
    virtual void submit(const int16_t* frames, size_t num_frames) = 0;   // interleaved int16
    virtual void close() = 0;
};
}
```
`src/audio/AudioPool.h`:
```cpp
#pragma once
#include <cstdint>
namespace rp { int audio_pick_free_slot(const bool* in_use, uint32_t n); }
```

- [ ] **Step 3: Run — verify fail; implement**

Run: `cmake --build build --config Debug` → FAIL (unresolved). `src/audio/AudioPool.cpp`:
```cpp
#include "audio/AudioPool.h"
namespace rp {
int audio_pick_free_slot(const bool* in_use, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (!in_use[i]) return (int)i;
    return -1;
}
}
```

- [ ] **Step 4: Wire CMake, build, run**

Top-level `CMakeLists.txt`: `target_sources(retropark PRIVATE src/audio/AudioPool.cpp)` (the `src` include dir is already on `retropark`). Add `test_audio_pool.cpp` to tests.
Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: pool tests pass; suite green.

- [ ] **Step 5: Commit**

```bash
git add src/audio/IAudioOutput.h src/audio/AudioPool.* tests/test_audio_pool.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: IAudioOutput interface + pooled free-slot helper"
```

---

## Task 2: `XAudio2Output`

**Files:**
- Create: `src/audio/XAudio2Output.h`, `src/audio/XAudio2Output.cpp`
- Modify: `CMakeLists.txt` (source + link `xaudio2`)

**Interfaces:**
- Consumes: `IAudioOutput`, `audio_pick_free_slot`.
- Produces: `class rp::XAudio2Output : public IAudioOutput` — `open` builds an XAudio2 engine + mastering voice + a source voice at `(sample_rate, channels)` 16-bit PCM; `submit` copies into a free pooled buffer and `SubmitSourceBuffer`s (drop if full); `OnBufferEnd` recycles; `close` stops/destroys. No standalone doctest (a gated device smoke lives in Task 5).

- [ ] **Step 1: Write the header**

`src/audio/XAudio2Output.h`:
```cpp
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include "audio/IAudioOutput.h"
#include <vector>
#include <mutex>

namespace rp {
class XAudio2Output : public IAudioOutput {
public:
    ~XAudio2Output() override;
    rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) override;
    void submit(const int16_t* frames, size_t num_frames) override;
    void close() override;
    void on_buffer_end(void* ctx);   // called by the voice callback

private:
    static const uint32_t kPoolSize = 16;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    struct VoiceCB* cb_ = nullptr;
    uint32_t channels_ = 2;
    bool com_inited_ = false;
    std::mutex mtx_;
    std::vector<std::vector<int16_t>> bufs_;   // kPoolSize
    bool in_use_[kPoolSize] = {false};
};
}
```

- [ ] **Step 2: Implement**

`src/audio/XAudio2Output.cpp`:
```cpp
#include "audio/XAudio2Output.h"
#include "audio/AudioPool.h"

namespace rp {

struct VoiceCB : public IXAudio2VoiceCallback {
    XAudio2Output* owner;
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override { owner->on_buffer_end(ctx); }
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

XAudio2Output::~XAudio2Output() { close(); }

rp_result XAudio2Output::open(uint32_t sample_rate, uint32_t channels, std::string& err) {
    channels_ = channels;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_inited_ = SUCCEEDED(hr);   // S_FALSE / RPC_E_CHANGED_MODE => someone else owns COM; don't uninit
    if (hr == RPC_E_CHANGED_MODE) com_inited_ = false;
    if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR))) { err="XAudio2Create"; return RP_ERR_DEVICE; }
    if (FAILED(engine_->CreateMasteringVoice(&master_))) { err="CreateMasteringVoice"; return RP_ERR_DEVICE; }
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = sample_rate; wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
    cb_ = new VoiceCB(); cb_->owner = this;
    if (FAILED(engine_->CreateSourceVoice(&source_, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, cb_))) {
        err="CreateSourceVoice"; return RP_ERR_DEVICE;
    }
    bufs_.assign(kPoolSize, {});
    source_->Start(0);
    return RP_OK;
}

void XAudio2Output::submit(const int16_t* frames, size_t num_frames) {
    if (!source_ || !frames || num_frames == 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(mtx_); slot = audio_pick_free_slot(in_use_, kPoolSize);
      if (slot < 0) return;                 // pool full -> drop
      in_use_[slot] = true; }
    size_t samples = num_frames * channels_;
    bufs_[slot].assign(frames, frames + samples);
    XAUDIO2_BUFFER b{};
    b.AudioBytes = (UINT32)(bufs_[slot].size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(bufs_[slot].data());
    b.pContext = reinterpret_cast<void*>((uintptr_t)slot);
    if (FAILED(source_->SubmitSourceBuffer(&b))) {
        std::lock_guard<std::mutex> lk(mtx_); in_use_[slot] = false;
    }
}

void XAudio2Output::on_buffer_end(void* ctx) {
    uint32_t slot = (uint32_t)(uintptr_t)ctx;
    std::lock_guard<std::mutex> lk(mtx_);
    if (slot < kPoolSize) in_use_[slot] = false;
}

void XAudio2Output::close() {
    if (source_) { source_->Stop(0); source_->FlushSourceBuffers(); source_->DestroyVoice(); source_=nullptr; }
    if (master_) { master_->DestroyVoice(); master_=nullptr; }
    if (engine_) { engine_->Release(); engine_=nullptr; }
    delete cb_; cb_=nullptr;
    if (com_inited_) { CoUninitialize(); com_inited_=false; }
}
}
```

- [ ] **Step 3: Wire CMake, build**

Top-level `CMakeLists.txt`: `target_sources(retropark PRIVATE src/audio/XAudio2Output.cpp)` and on Windows `target_link_libraries(retropark PUBLIC xaudio2)`.
Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: builds and links; the full suite stays green (nothing calls XAudio2Output yet).

- [ ] **Step 4: Commit**

```bash
git add src/audio/XAudio2Output.* CMakeLists.txt
git commit -m "feat: XAudio2Output (source-voice playback + pooled buffers + recycle)"
```

---

## Task 3: Runtime audio wiring + diagnostic accessor

**Files:**
- Modify: `include/retropark/retropark.h`, `src/runtime/Runtime.h`, `src/runtime/Runtime.cpp`
- Modify: `tests/test_runtime_api.cpp`

**Interfaces:**
- Consumes: `IAudioOutput`, `XAudio2Output`, `rp_av_info`.
- Produces:
  - C API `void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out);`
  - Runtime installs a real `audio_sample` trampoline that counts + forwards; opens audio when a driven core reports `sample_rate > 0` (best-effort); closes on unload.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_runtime_api.cpp`:
```cpp
TEST_CASE("runtime: audio stats are zero for a no-audio driven core") {
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 64, 64) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_DRIVEN_CORE_DIR) == RP_OK);   // refcore_driven: sample_rate 0
    std::vector<uint8_t> img(64*64*4, 0);
    for (int i=0;i<5;i++) rp_runtime_present(rt, img.data());
    uint64_t frames = 999; int nonsilent = 9;
    rp_runtime_audio_stats(rt, &frames, &nonsilent);
    CHECK(frames == 0);          // no-audio core produced no samples
    CHECK(nonsilent == 0);
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
```

- [ ] **Step 2: Public header decl**

`include/retropark/retropark.h` (inside the `extern "C"` block):
```c
/* Diagnostics for the audio path (test/telemetry): frames_out = total stereo frames the runtime
   received from the core since load; nonsilent_out = 1 if any non-near-zero sample was seen. */
void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out);
```

- [ ] **Step 3: Runtime members + trampoline + open/close**

`src/runtime/Runtime.h`: include `"audio/IAudioOutput.h"`; add members:
```cpp
std::unique_ptr<IAudioOutput> audio_;
uint64_t audio_frames_ = 0;
bool audio_nonsilent_ = false;
void on_audio_sample(const int16_t* frames, size_t num_frames);
void open_audio(const rp_av_info& av);
```
`src/runtime/Runtime.cpp`:
- Replace the no-op `host_audio_sample` trampoline body with a forward:
```cpp
static void host_audio_sample(rp_host* h, const int16_t* f, size_t n) {
    reinterpret_cast<Runtime*>(h)->on_audio_sample(f, n);
}
```
(the constructor already installs `host_iface_.audio_sample = host_audio_sample;` — keep that.)
- Implement:
```cpp
#include "audio/XAudio2Output.h"
void Runtime::on_audio_sample(const int16_t* frames, size_t n) {
    if (!frames || n == 0) return;
    audio_frames_ += n;
    if (!audio_nonsilent_) {
        for (size_t i = 0; i < n * 2; ++i) { int16_t s = frames[i]; if (s > 128 || s < -128) { audio_nonsilent_ = true; break; } }
    }
    if (audio_) audio_->submit(frames, n);
}
void Runtime::open_audio(const rp_av_info& av) {
    if (av.sample_rate <= 0.0) return;           // no audio
    auto out = std::make_unique<XAudio2Output>();
    std::string err;
    if (out->open((uint32_t)av.sample_rate, 2, err) == RP_OK) audio_ = std::move(out);
    // best-effort: on failure leave audio_ null (game runs silent); do NOT fail load
}
```
- Where AV info is validated for a driven core, call `open_audio(av)`: in `load_content` (content cores, after the av validation) and in the no-content driven branch of `load_core` (after its av validation). Reset `audio_frames_=0; audio_nonsilent_=false;` at the start of a load and open.
- In `unload_core`: `if (audio_) { audio_->close(); audio_.reset(); } audio_frames_=0; audio_nonsilent_=false;`.
- C API:
```cpp
void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out) {
    auto* r = reinterpret_cast<Runtime*>(rt);
    if (frames_out) *frames_out = r->audio_frames();      // add a trivial getter, or friend the C fn
    if (nonsilent_out) *nonsilent_out = r->audio_nonsilent() ? 1 : 0;
}
```
(Add `uint64_t audio_frames() const { return audio_frames_; } bool audio_nonsilent() const { return audio_nonsilent_; }` to Runtime.)

- [ ] **Step 4: Build and run**

Run: `cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the no-audio-core stats test passes (frames==0); the whole A–D suite stays green (the reference driven core reports `sample_rate 0`, so no audio opens; presenting cores never call audio_sample).

- [ ] **Step 5: Commit**

```bash
git add include/retropark/retropark.h src/runtime/Runtime.* tests/test_runtime_api.cpp
git commit -m "feat: runtime audio wiring (open on load, forward+count audio_sample) + diagnostics"
```

---

## Task 4: Shim forwards audio

**Files:**
- Modify: `cores/libretro_shim/LibretroShim.cpp`

**Interfaces:**
- Produces: the shim's libretro audio callbacks forward to `host->audio_sample`; `GET_AUDIO_VIDEO_ENABLE` reports audio enabled.

- [ ] **Step 1: Forward the audio callbacks**

In `cores/libretro_shim/LibretroShim.cpp`, replace the dropping audio callbacks:
```cpp
// batch: interleaved stereo int16, `frames` stereo frames. Forward straight through.
size_t audio_batch_cb(const int16_t* data, size_t frames) {
    if (g && data && frames) g->host.audio_sample(g->host.host, data, frames);
    return frames;
}
// per-sample: forward one stereo frame.
void audio_sample_cb(int16_t left, int16_t right) {
    if (g) { int16_t pair[2] = {left, right}; g->host.audio_sample(g->host.host, pair, 1); }
}
```
(Ensure both are wired via `retro_set_audio_sample(audio_sample_cb)` and `retro_set_audio_sample_batch(audio_batch_cb)` in `sh_create` — they already are; only the bodies change.)

- [ ] **Step 2: GET_AUDIO_VIDEO_ENABLE reports audio on**

In `env_cb`, the `RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE` handler: set the result so **audio is enabled** (bit 0) and video enabled (bit 1) — i.e. `*(int*)data = 0x3;` (or the appropriate bits per libretro.h). This is now truthful since audio plays.

- [ ] **Step 3: Build — confirm the shim still loads FCEUmm**

Run: `cmake --build build --config Debug` and confirm the shim + fceumm + core.json still emit. The full existing suite stays green (behavior proven in Task 5). If you can, re-run the existing libretro e2e to confirm the video path is unregressed (the audio forwarding must not break `retro_run`).

- [ ] **Step 4: Commit**

```bash
git add cores/libretro_shim/LibretroShim.cpp
git commit -m "feat: libretro shim forwards audio to the host (was dropped)"
```

---

## Task 5: Audio-flow e2e (gated) + device smoke

**Files:**
- Create: `tests/test_audio_flow.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: the C API + `rp_runtime_audio_stats` + the emitted `libretro_shim` package (FCEUmm) + a ROM; `XAudio2Output` for the device smoke.

- [ ] **Step 1: Write the tests**

`tests/test_audio_flow.cpp` — reuse the `RP_SHIM_DIR` / `RP_NES_ROM_DIR` compile-defs and the `first_nes`/`file_exists` helpers (copy the small helpers, or share via a header). Two cases:
```cpp
TEST_CASE("audio flow: FCEUmm produces a plausible non-silent stereo stream") {
    std::string rom = first_nes(RP_NES_ROM_DIR);
    if (rom.empty() || !file_exists(std::string(RP_SHIM_DIR)+"/fceumm_libretro.dll")) { WARN("no core/rom; skip"); return; }
    rp_runtime* rt = rp_runtime_create(RP_GFX_D3D11, nullptr);   // audio path is backend-independent
    REQUIRE(rt);
    REQUIRE(rp_runtime_resize(rt, 256, 240) == RP_OK);
    REQUIRE(rp_runtime_load_core(rt, RP_SHIM_DIR) == RP_OK);
    REQUIRE(rp_runtime_load_content(rt, rom.c_str()) == RP_OK);
    std::vector<uint8_t> img(256*240*4, 0);
    for (int i = 0; i < 120; ++i) rp_runtime_present(rt, img.data());
    uint64_t frames = 0; int nonsilent = 0;
    rp_runtime_audio_stats(rt, &frames, &nonsilent);
    // ~ sample_rate/fps * 120 stereo frames; NES ~ (48000/60)*120 = 96000. Assert a broad band.
    CHECK(frames > 20000);           // clearly audio flowed (not zero / not a trickle)
    CHECK(nonsilent == 1);           // the game actually produced sound, not silence
    rp_runtime_unload_core(rt); rp_runtime_destroy(rt);
}
TEST_CASE("audio device: XAudio2 opens, plays a buffer, closes (gated)") {
    rp::XAudio2Output out; std::string err;
    if (out.open(48000, 2, err) != RP_OK) { WARN("no audio device; skip"); return; }
    std::vector<int16_t> tone(48000*2/10, 0);     // 0.1s of silence (submit path only)
    out.submit(tone.data(), tone.size()/2);
    out.close();
    CHECK(true);                     // reaching here without crashing is the assertion
}
```
Include `"render/..."` is NOT needed; include `<retropark/retropark.h>` and `"audio/XAudio2Output.h"`. The audio flow test uses `RP_GFX_D3D11` (WARP) since the audio path is independent of the GPU backend — no need to run both.

- [ ] **Step 2: Wire CMake**

`tests/CMakeLists.txt`: the test needs the shim package + rom dir defs (already present from Slice D as `RP_SHIM_DIR`/`RP_NES_ROM_DIR`) and `add_dependencies(retropark_tests LibretroShim)` (already present). Add `test_audio_flow.cpp`. If the `first_nes`/`file_exists` helpers were defined in `test_libretro_e2e.cpp` with internal linkage, duplicate the small helpers here (or extract to a shared test header) to avoid ODR issues.

- [ ] **Step 3: Build and run — verify (or SKIP)**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: the audio-flow test RUNS (FCEUmm + a ROM present) and passes — FCEUmm produced tens of thousands of non-silent stereo frames that reached the runtime; the device smoke opens/plays/closes (or skips if no device). **Report the actual `frames`/`nonsilent` values and whether the device smoke ran or skipped.** The whole A–D suite stays green.

- [ ] **Step 4: Harness — hear it (manual)**

Launch `retropark_harness.exe --api d3d11 --content "<a NES rom>"` (foreground, with audio) and confirm you HEAR the game's audio (NES music/sfx). Audio can't be screenshotted; note in the report whether audio was audible. (Do not block the commit on a headless machine with no device — the automated flow test is the gate.)

- [ ] **Step 5: Commit**

```bash
git add tests/test_audio_flow.cpp tests/CMakeLists.txt
git commit -m "test: gated audio-flow e2e (FCEUmm non-silent stream) + XAudio2 device smoke"
```

---

## Self-Review

**Spec coverage:**
- §1 IAudioOutput + XAudio2Output (resamples; pooled + OnBufferEnd + drop-on-full; mutex) → Task 1 (interface+pool) + Task 2 (XAudio2). ✓
- §1 runtime: audio_ output, on_audio_sample forward+count, open on driven load (sample_rate>0) best-effort, close on unload → Task 3. ✓
- §1 diagnostic rp_runtime_audio_stats → Task 3. ✓
- §1 shim forwards audio + GET_AUDIO_VIDEO_ENABLE audio-on → Task 4. ✓
- §2 data flow (run_frame → shim audio_batch_cb → host->audio_sample → on_audio_sample → submit → XAudio2) → Tasks 3+4, exercised in Task 5. ✓
- §3 error handling: open failure non-fatal (Task 3 open_audio leaves audio_ null), sample_rate==0 no audio (Task 3), pool drop-on-full (Task 2 submit), null/zero skip (Task 3 on_audio_sample), teardown stop+flush (Task 2 close), mutex-guarded pool (Task 2). ✓
- §4 testing: pool unit (Task 1), device-independent audio-flow e2e via counter (Task 5), device-open smoke gated (Task 5), harness audible (Task 5 manual), A–D regression (every task). ✓
- §5 scope: all in-scope built; presenting-core audio / A-V sync / resampler / volume / 2nd backend NOT built. ✓
- No core-ABI change: `RETROPARK_ABI_VERSION` untouched; only `rp_runtime_audio_stats` added to the C API. ✓

**Placeholder scan:** the runtime C-API getter note ("add a trivial getter, or friend the C fn") in Task 3 is spelled out with the exact getters to add — not a vague TODO. XAudio2 code is complete (real `Vk`-free Win32/XAudio2 calls). No "add error handling"-style placeholders.

**Type consistency:** `IAudioOutput::open(uint32_t,uint32_t,std::string&)/submit(const int16_t*,size_t)/close()`, `audio_pick_free_slot(const bool*,uint32_t)`, `rp_runtime_audio_stats(rp_runtime*,uint64_t*,int*)`, `Runtime::on_audio_sample(const int16_t*,size_t)`/`open_audio(const rp_av_info&)`/`audio_frames()`/`audio_nonsilent()`, and the shim `audio_batch_cb(const int16_t*,size_t)`/`audio_sample_cb(int16_t,int16_t)` are used identically across Tasks 1–5. `channels = 2` throughout.
```
