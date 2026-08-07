# RetroPark — Slice E Design (audio output)

**Date:** 2026-08-07
**Status:** Approved (design), pending implementation plan
**Scope:** Play a driven libretro core's audio (NES sound) through an OS audio device — wiring
the already-declared `audio_sample` callback to a new XAudio2-backed audio output. No ABI change.

---

## 0. Context and goal

Audio has been deferred through Slices A–D. The core ABI already declares a host callback
`rp_host_iface.audio_sample(host, const int16_t* frames, size_t num_frames)` (Slice C), currently
a no-op in the runtime, and the libretro shim currently *drops* the libretro audio callbacks. The
sample rate already flows through `get_av_info` (`rp_av_info.sample_rate`).

Slice E makes RetroPark actually *play* that audio: the libretro shim forwards audio to the host,
and the runtime plays it via a new `IAudioOutput` (XAudio2). Target: **driven-core (libretro)
audio** — you hear the NES game (Donkey Kong). Presenting-core audio (heavy apps do their own) is
deferred. It is a runtime + module addition with **no core-ABI change**.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Source | **Driven-core (libretro) audio only.** Presenting-core audio deferred. |
| Backend | **XAudio2** behind an `IAudioOutput` interface (native, no vendored dependency). A source voice **resamples internally**, so we never write a resampler. A portable backend is a later follow-on behind the interface. |
| Sync | **Feed-as-produced + buffer.** Audio-master-clock A/V sync / fps pacing deferred (some drift acceptable for the "it has sound" milestone). |
| ABI | **No core-ABI change** (audio_sample exists, rate from get_av_info). One *additive* diagnostic C API (`rp_runtime_audio_stats`) for the device-independent test. |
| Failure | Audio is **best-effort**: no device / init failure runs the game silently, never fails load or breaks playback. |

---

## 1. Components (no ABI change)

**`src/audio/IAudioOutput.h`** — the interface:
```cpp
struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) = 0;
    virtual void submit(const int16_t* frames, size_t num_frames) = 0;   // interleaved int16
    virtual void close() = 0;
};
```

**`src/audio/XAudio2Output.{h,cpp}`** — the one implementation:
- `open`: `XAudio2Create` → `IXAudio2MasteringVoice` → an `IXAudio2SourceVoice` with
  `WAVEFORMATEX{WAVE_FORMAT_PCM, channels, sample_rate, 16-bit}`; `Start`. XAudio2 resamples the
  source voice's rate to the device rate.
- `submit`: copy `num_frames*channels` int16 into a **pooled buffer**, `SubmitSourceBuffer`. A
  bounded pool of N buffers is recycled via an `IXAudio2VoiceCallback::OnBufferEnd` callback (a
  submitted buffer must stay alive until XAudio2 finishes it). If no buffer is free (playback fell
  behind) → **drop this batch** (brief glitch), never block. The pool is mutex-guarded (submit runs
  on the present thread; OnBufferEnd on XAudio2's thread).
- `close`: `Stop` + `FlushSourceBuffers`, destroy source + mastering voices, release the engine.

A tiny pure helper (`audio_pick_free_slot(const bool* in_use, uint32_t n) -> int`, returns a free
index or -1) is factored out and unit-tested (the drop-on-full decision), so the pool logic has a
deterministic test independent of XAudio2.

**`src/runtime/Runtime.{h,cpp}`** — owns a `std::unique_ptr<IAudioOutput> audio_;`:
- On driven-core load (after content/av-info gives the rate): if `av.sample_rate > 0`, create an
  `XAudio2Output` and `open(sample_rate, 2, err)`. **If open fails, log and leave `audio_` inert**
  (game runs silent) — do NOT fail `load_core`.
- The `audio_sample` trampoline (today `host_audio_sample` no-op) → `on_audio_sample(frames, n)`:
  increments `audio_frames_submitted_ += n`, sets `audio_nonsilent_` if any |sample| exceeds a
  small threshold, and calls `audio_->submit(frames, n)` if `audio_` is open. Skips null/zero.
- `unload_core`/destroy: `audio_->close()` + reset; reset the counters.

**Diagnostic C API (additive, for the device-independent test):**
```c
/* Diagnostics for the audio path (test/telemetry). frames_out = total stereo frames the runtime
   received from the core since load; nonsilent_out = 1 if any non-near-zero sample was seen. */
void rp_runtime_audio_stats(rp_runtime* rt, uint64_t* frames_out, int* nonsilent_out);
```

**`cores/libretro_shim/LibretroShim.cpp`** — stop dropping audio:
- `audio_batch_cb(const int16_t* data, size_t frames)` → `host->audio_sample(host, data, frames)`;
  return `frames`.
- `audio_sample_cb(int16_t l, int16_t r)` → accumulate the L/R pair and forward as one frame
  (`int16_t pair[2]={l,r}; host->audio_sample(host, pair, 1);`). (FCEUmm uses the batch callback;
  the per-sample path is handled for completeness.)
- `GET_AUDIO_VIDEO_ENABLE` now reports **audio enabled** (it actually plays now), resolving the
  Slice-D Minor where it claimed enabled while dropping.

## 2. Data flow (audio, per frame)

1. **Load:** driven core loads; `get_av_info` → `sample_rate`. If `> 0`, runtime opens
   `XAudio2Output(sample_rate, 2)` (best-effort).
2. **Run:** `present()` → `run_frame` → FCEUmm calls the shim's `audio_batch_cb(data, frames)` →
   shim forwards `host->audio_sample(data, frames)` → runtime `on_audio_sample` counts + submits →
   XAudio2 plays (resampling to the device).
3. **Teardown:** unload closes the audio output.

Video is unchanged (Slice C driven path). Audio and video are both produced inside `retro_run`;
they're fed independently (video via `composite_driven`, audio via `submit`), no cross-locking.

## 3. Error handling

- **No device / `XAudio2Create` or voice-create fails / `open` fails** → `open` returns an error;
  the runtime logs it, leaves `audio_` inert, and continues (silent game). Never fatal.
- **`sample_rate == 0`** → no audio opened; trampoline inert (still counts nothing).
- **Pool exhaustion** → drop the batch, no block/crash (bounded pool + drop-on-full).
- **Null/zero-length `audio_sample`** → skip.
- **Teardown with buffers queued** → `Stop`+`FlushSourceBuffers` before `DestroyVoice`.
- **Threading** → the free-buffer pool is mutex-guarded across the present thread (submit) and the
  XAudio2 thread (`OnBufferEnd`).
- **Crash honesty** → unchanged (in-process; a bad core can still fault the host).

## 4. Testing

- **Pure/deterministic:** `audio_pick_free_slot` (drop-on-full / free-slot selection) unit-tested
  with no device.
- **Audio-flow e2e (gated on FCEUmm+ROM, device-INdependent):** load FCEUmm + a ROM, run ~120
  frames, then `rp_runtime_audio_stats` → assert `frames` is in a plausible band (roughly
  `sample_rate/fps × 120`, within a generous tolerance) **and** `nonsilent == 1` (the game really
  produced sound, not zeros). Proves shim → runtime audio flow end-to-end with **no device and no
  "listening"**. `WARN`-skips if the core/ROM are absent (like the libretro e2e).
- **Device-open smoke (gated on an audio device):** `XAudio2Output out; out.open(48000, 2, err)` →
  submit a short buffer → `close` — no crash. If `open` fails (no device / headless), `WARN`-skip.
- **Harness (human proof):** `--content <rom>` now plays sound — you hear the NES game. Manual
  (audio can't be screenshotted); the automated proof is the flow counter.
- **Regression:** the whole A–D suite stays green — audio is additive, no core-ABI change; the shim
  now forwarding audio is exactly what the flow e2e verifies.

## 5. Scope

**In Slice E:**
- `IAudioOutput` + `XAudio2Output` (XAudio2 resamples; pooled buffers + `OnBufferEnd` recycling +
  drop-on-full; mutex-guarded).
- Runtime: open audio on driven-core load (`sample_rate > 0`), forward `audio_sample` → submit +
  count, close on unload; **audio failure non-fatal**.
- Diagnostic `rp_runtime_audio_stats` (additive C API).
- Shim forwards audio callbacks; `GET_AUDIO_VIDEO_ENABLE` reports audio on.
- `audio_pick_free_slot` unit test + gated audio-flow e2e + gated device-open smoke; harness plays
  audio.

**Explicitly out (deferred):**
- Presenting-core audio (heavy apps do their own).
- Audio-master-clock A/V sync / fps pacing (feed-as-produced).
- Us writing a resampler (XAudio2 does it); a portable/second audio backend (interface leaves room).
- Volume/mute, audio device selection, per-core audio options.
- The standing list: savestate/rewind, more validated systems, cross-API interop, wrapping real
  heavy apps, out-of-process isolation, iOS/Android, EverythingBox integration.

**The single provable claim of Slice E:** *a driven libretro core's audio (FCEUmm/NES) flows
through the shim → runtime → XAudio2 and plays — proven by a gated, device-independent e2e
asserting a plausible, non-silent stereo stream reaches the runtime, a device-open smoke, and
audible NES sound in the harness — with the whole A–D suite unaffected.*

## 6. Repo additions

```
src/audio/
  IAudioOutput.h                       # interface
  XAudio2Output.h/.cpp                 # XAudio2 impl + pooled buffers + OnBufferEnd recycle
  AudioPool.h/.cpp                     # audio_pick_free_slot (pure, tested)
include/retropark/retropark.h          # + rp_runtime_audio_stats (additive diagnostic)
src/runtime/Runtime.h/.cpp             # audio_ output, on_audio_sample forward+count, open/close
cores/libretro_shim/LibretroShim.cpp   # forward audio callbacks; GET_AUDIO_VIDEO_ENABLE -> audio on
tests/
  test_audio_pool.cpp                  # pick_free_slot / drop-on-full (pure)
  test_audio_flow.cpp                  # gated audio-flow e2e (device-independent counter) + device smoke
```
