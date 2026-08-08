# RetroPark — Slice L Design (Dolphin audio, host-owned)

**Date:** 2026-08-08
**Status:** Approved (design)
**Scope:** Route standalone Dolphin's game audio through RetroPark's XAudio2 output (the Slice-E
`audio_sample` path) so the **host owns the final A/V mix** — the audio analogue of the presenting
video model, where the host owns the final composite. Any GameCube ISO played through the
`dolphin_present` core (Slice K) now produces sound. **No core-ABI change** (ABI stays v5).

---

## 0. Context and goal

Slices I–K broke Dolphin from libretro and made it a reusable RetroPark presenting core: the Runtime
loads `dolphin_present`, hands it a shared `VkImage`, and drives `present()` with a composited overlay
— but **silent** (Slice K explicitly deferred audio). Slice E built RetroPark's audio output for
*driven* cores: an `audio_sample` host callback → `XAudio2Output` (a source voice that resamples
internally), plus device-independent `rp_runtime_audio_stats` counters. Slice E explicitly deferred
"presenting-core audio (the Slice E `audio_sample` path for a presenting core)."

Slice L closes that gap for Dolphin. Dolphin's DSP already mixes game audio into a `Mixer` at 48 kHz
interleaved stereo s16 — **byte-for-byte the format `rp_host.audio_sample` and `XAudio2Output` already
consume**. So the work is: pull Dolphin's mix and forward it over the existing host callback, and open
RetroPark's audio for the presenting core. The host owns the final output: unified volume/mute later,
a single device, and out-of-process-ready (PCM over the ABI is trivial to ship over a pipe).

### Decisions

| Decision | Choice |
|---|---|
| Ownership | **Host-owned.** Dolphin's mix → `rp_host.audio_sample` → RetroPark's `XAudio2Output`. Not Dolphin's own device backend. |
| Producer mechanic | **In-vehicle mixer-pull** (all in `rp_dolphin.cpp`): run Dolphin with the **Null** audio backend (no device), force the mixer output rate back to 48000, and pull `Mixer::Mix()` on a dedicated thread, forwarding each chunk. No new Dolphin source files, no `DolphinLib.props` edits. |
| Rate delivery | Reuse the **existing `get_av_info` hook**: wire `dp_get_av_info` to report `sample_rate = mixer rate (48000)`, channels 2. The Runtime reads it to open audio at the right rate. No ABI change. |
| Consumer | **Unchanged.** `Runtime::on_audio_sample → XAudio2Output` is already wired from Slice E. |
| Sync | **Deferred.** Stereo s16 48 kHz, accept minor A/V drift (Dolphin is throttled to the present rate ≈ realtime; XAudio2 buffering absorbs jitter). No audio-driven emulation-speed sync in this slice. |

---

## 1. Components

### Dolphin producer — `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp` (patch)
- **No device output.** Seed `Config::MAIN_AUDIO_BACKEND = BACKEND_NULLSOUND` before boot so Dolphin
  opens no audio device. `SendAIBuffer` still pushes the DSP output into the SoundStream's `Mixer`
  regardless of backend (verified), so the mix is available to pull.
- **Restore the mixer rate.** `NullSound::Init()` sets the mixer output rate to 0 ("audio disabled");
  after boot/`InitSoundStream`, force `system.GetSoundStream()->GetMixer()->SetSampleRate(48000)` so
  `Mix()` produces a valid 48 kHz stream.
- **Audio-puller thread.** A dedicated thread that steadily pulls `GetMixer()->Mix(buf, N)` (interleaved
  stereo s16) paced to ~48 kHz wall-clock (e.g. ~480-frame / ~10 ms chunks) and forwards each chunk via
  `rp_host.audio_sample(host, buf, n_frames)` when a host with a non-null `audio_sample` is set (the
  Slice-K `submit_frame` reporting mode). Started after boot in `dp_start`; signalled to stop and
  **joined** in `dp_stop`/`dp_destroy` **before** the SoundStream/mixer is torn down.
- **Report the rate.** Wire `dp_get_av_info` (currently `nullptr` in `kAbi`) to fill `rp_av_info` with
  `sample_rate = GetMixer()->GetSampleRate()` (48000) and `num_channels = 2`; geometry fields 0 (this
  core is presenting — geometry is not driven-validated). This is how the Runtime learns to open audio.

### RetroPark Runtime — `src/runtime/Runtime.cpp`
- After a **presenting content** core is started (the Slice-K `load_content` path), if the core exposes
  `get_av_info` and reports `sample_rate > 0`, call the existing `open_audio(av)`. `open_audio` opens
  `XAudio2Output` at that rate with the fixed stereo (2-channel) pipeline contract; `on_audio_sample`
  already forwards to it. Best-effort: no device → silent, never fatal (Slice-E behavior).
- Teardown: `unload_core()` already closes `audio_`. Ordering: the core's `dp_stop`/`dp_destroy` joins
  the puller (stops calling `audio_sample`) as part of `loader_.destroy()`, then the Runtime closes
  `audio_` — no callback races.

### No ABI change
`audio_sample` (host callback) and `get_av_info` (core hook) have existed since Slice C. ABI stays **v5**.

## 2. Data flow (steady state)

```
Dolphin emulation (throttled to the present lock-step ≈ realtime)
  DSP -> AudioCommon::SendAIBuffer -> Mixer::PushSamples        (Dolphin's thread)
  puller thread: Mixer::Mix(buf, ~480)  (48 kHz s16 stereo)
     -> rp_host.audio_sample(host, buf, ~480)
        -> Runtime::on_audio_sample -> XAudio2Output::submit
           -> source voice (resamples to device) -> speakers
```
Host-owned and unified — the same `XAudio2Output` that plays driven/libretro audio.

## 3. Error handling / robustness

- No audio device → `XAudio2Output` open is best-effort; the game is silent but never crashes, and the
  device-independent stats still count (Slice-E contract).
- Puller lifecycle: a stop flag + join guarantees the thread never touches a destroyed mixer; the puller
  no-ops until a host with `audio_sample` is set (so the Slice-J direct-C-API mode stays silent, unchanged).
- Mixer underrun/overrun from emulation-speed vs pull-rate mismatch is handled by Dolphin's `Mix()`
  (stretches/pads) + XAudio2's drop-on-full queue — no crash, at worst minor artifacts (the deferred sync).
- COM: `XAudio2Output` handles `CoInitializeEx` as in Slice E (unchanged); the presenting path reuses it.

## 4. Testing

- **Gated e2e (real Dolphin, device-independent):** extend the Slice-K Runtime path — `rp_runtime_create`
  (RP_GFX_VULKAN) + `load_core(dolphin_present)` + `load_content(Billy Hatcher)` + drive `rp_runtime_present`
  for a few seconds — then assert `rp_runtime_audio_stats` reports `frames > 0` **and** non-silence
  (`nonsilent`), proving Dolphin's audio reached the host through `audio_sample`. Counts even with no
  output device (Slice-E stats are device-independent). Opt-in `RP_RUN_DOLPHIN=1`, WARN-skips without
  GPU / DLL / ROM. **Add the audio-stats assertion to the existing `test_dolphin_core_e2e.cpp`** so the
  single Dolphin boot proves video *and* audio (avoids a second ~30 s boot); the existing video/overlay
  assertions stay.
- **Harness (human proof):** the windowed `--content <iso>` run plays audible Billy Hatcher audio.
- **Regression:** full A–K suite green; the video handoff/overlay assertions still pass; the Slice-J
  direct-C-API mode stays silent (no `audio_sample` host set), unchanged.

## 5. Scope

**In Slice L:** the in-vehicle mixer-pull producer (Null backend + rate restore + puller thread +
`dp_get_av_info`), the Runtime opening audio for a presenting core that reports a rate, the gated
device-independent audio-stats e2e, and audible audio in the harness. **No ABI change.**

**Out (later):** A/V-sync / audio-driven emulation-speed sync / audio stretching, volume/mute/
device-selection, surround / DPL2, Wii-speaker and GBA audio, rewinding/savestate of audio,
presenting-core audio for cores other than Dolphin, out-of-process audio transport.

**The single provable claim:** *Dolphin's real game audio reaches RetroPark's XAudio2 output through
`rp_host.audio_sample`, driven through `rp_core_abi` — you hear the GameCube game, host-owned, no
libretro — proven by a gated device-independent audio-stats assertion (frames flowing + non-silent).*

## 6. Repo additions

```
external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp  # Null backend, rate restore, puller thread,
                                                          #   dp_get_av_info -> sample_rate (patch)
src/runtime/Runtime.cpp                                   # open audio for a presenting core reporting a rate
tests/test_dolphin_core_e2e.cpp                           # + gated device-independent audio-stats assertion
docs/patches/dolphin-external-present.patch               # refreshed with the audio producer changes
```
