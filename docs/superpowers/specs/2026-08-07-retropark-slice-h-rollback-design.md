# RetroPark — Slice H Design (rollback netplay, GGPO-style, 1v1, driven cores)

**Date:** 2026-08-07
**Status:** Approved (design), pending implementation plan
**Scope:** Rollback (GGPO-style) netplay for driven cores — predict the remote input, run ahead, and
when a real remote input arrives that differs from the prediction, load a savestate and **re-simulate
silently** to correct. Builds on Slice G's lockstep transport/protocol and Slice F's fast per-frame
save/load. 1v1, LAN-direct (reuses TCP), driven cores only. No core-ABI change.

---

## 0. Context and goal

Slice G shipped delay-based **lockstep** netplay: both machines exchange inputs and only advance when
both are in. Lockstep is simple and correct but every frame waits on the slowest link, so latency is
felt directly. **Rollback** hides latency: each machine **predicts** the remote input (repeat-last),
runs ahead without waiting, and when the real remote input arrives and differs, **rolls back** to a
saved state and **re-simulates** the intervening frames with the corrected input. Because the core is
deterministic (Slice F/G invariant), the re-simulated confirmed state provably matches what a lockstep
run of the same inputs would produce — the correction is exact.

Rollback needs two things RetroPark already has and one it doesn't:
- **Fast per-frame save/load** — `rp_runtime_serialize_size`/`save_state`/`load_state` (Slice F). ✓
- **A transport + input protocol** — `ITransport`/`TcpTransport`/`NetProtocol` (Slice G). ✓
- **The ability to simulate a frame silently and invisibly** — today `present()` fuses `run_frame`
  (which is also when the core emits audio) with `composite` (render). Re-simulation must run the core
  forward feeding corrected inputs while emitting **no audio** and rendering only the **final** frame.
  This slice adds that split.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Model | **Rollback (GGPO-style)**, predict + resimulate. Delay-based lockstep is Slice G. |
| Sim/present split | **Additive C API**: `rp_runtime_advance(rt, emit_audio)` (run_frame, no composite) + `rp_runtime_render(rt, out)` (composite last frame). `present()` = advance(1)+render. Audio suppressed during resim. **No core-ABI change** (ABI stays v5). |
| Prediction | **Repeat-last-known remote input** (neutral until the first arrives). |
| Prediction window | **`max_prediction = 8`** frames ahead of the confirmed remote frame; beyond that, **stall** (bounded), don't run further ahead. |
| Architecture | **Separate `RollbackSession`** (keeps the lockstep `NetSession` clean), sharing `ITransport`/`NetProtocol`/`crc32`. |
| Transport | **Reuse `TcpTransport`** (no UDP this slice). A **test-only `DelayTransport`** wrapper forces mispredictions deterministically in-process. |
| Proof rigor | **Add a tiny input-sensitive driven core** (`refcore_rollback`) so mispredictions genuinely diverge and resim genuinely corrects — the portable gate asserts convergence to a lockstep ground truth, byte-exact, with `rollback_count > 0`. |
| Desync | Checksum **confirmed frames only** (predicted frames legitimately differ until corrected). |

---

## 1. Components

### Runtime sim/present split (`src/runtime/Runtime.{h,cpp}`, `include/retropark/retropark.h`)
Additive C API — **no core-ABI change**:
```c
/* Advance the driven core one frame (run_frame) WITHOUT compositing. emit_audio!=0 forwards the
   frame's audio to the output; 0 suppresses it (used for silent re-simulation during rollback).
   The advanced framebuffer is retained for a subsequent rp_runtime_render. */
rp_result rp_runtime_advance(rp_runtime* rt, int emit_audio);
/* Composite the last-advanced driven framebuffer to out_rgba (and the display). */
rp_result rp_runtime_render(rp_runtime* rt, uint8_t* out_rgba);
```
- `advance(emit_audio)`: the driven branch of the current `present()` minus the composite — captures a
  rewind snapshot if enabled (unchanged), runs `run_frame`, stores the framebuffer (`dr_*`), and sets a
  `suppress_audio_` flag around `run_frame` so `on_audio_sample` submits only when `emit_audio`.
- `render(out)`: the composite tail of `present()` — `composite_driven(dr_data_ …)` with the validity
  logic unchanged.
- `present(out)` becomes `advance(1)` then `render(out)` — single-player / lockstep behavior is
  byte-identical (same run_frame → composite order, audio on).
- `on_audio_sample` gains a `if (suppress_audio_) return;` guard (before counting/submitting). The
  audio counters (`rp_runtime_audio_stats`) count only emitted audio, so resim frames don't inflate
  them.
- **Presenting cores:** out of scope (rollback is driven-only, like all netplay). `advance`/`render`
  on a presenting core → `RP_ERR_UNSUPPORTED` (or route present's presenting tail unchanged; the
  rollback session only drives driven cores).

### `RollbackSession` (`src/net/RollbackSession.{h,cpp}`)
Owns, against a `Runtime&` + `ITransport&`:
- `state_ring_`: `std::map<uint64_t, std::vector<uint8_t>>` frame → pre-frame serialized blob, bounded
  to `max_prediction_ + slack`.
- `local_inputs_`, `remote_inputs_`, `predicted_inputs_`: `std::map<uint64_t, rp_input_state>` histories.
- `frame_` (next frame to simulate), `confirmed_` (highest frame with a real remote input),
  `verified_` (highest frame whose remote prediction has been checked/corrected).
- `local_port_`/`remote_port_` (host 0/1, join 1/0 — same as Slice G), `max_prediction_` (default 8),
  `rollback_count_` (telemetry).

API:
```cpp
enum class RbStatus { Ok, Stalled, Desync, Disconnected };
rp_result start_host(Runtime&, ITransport&, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err);
rp_result start_join(Runtime&, ITransport&, uint64_t content_hash, const char* core_id, std::string& err);
RbStatus  tick(const rp_input_state& local_now, uint8_t* out_rgba);   // one displayed frame
uint64_t  frame() const; uint64_t confirmed_frame() const; uint64_t rollback_count() const;
RbStatus  status() const;
```
Handshake + initial `serialize()`→`STATE_SYNC`→`load_state` is identical to Slice G (host authoritative;
join adopts config from the host Hello). `HELLO` carries `max_prediction` instead of `input_delay`.

### Prediction (`repeat-last`)
`predict_remote(frame)` = `remote_inputs_[confirmed_]` if any confirmed input exists, else a zeroed
neutral. A tiny pure helper `rb_predict(const std::map&, uint64_t confirmed) -> rp_input_state` is
factored out and unit-tested.

### Misprediction detection
`first_mispredicted(from, to)` scans confirmed frames in `[from, to]` for the earliest frame whose
**real** `remote_inputs_[g]` differs (`memcmp`) from `predicted_inputs_[g]` (what we actually fed). A
pure helper, unit-tested. Returns the frame or a sentinel (no misprediction).

### `DelayTransport` (test-only, `tests/`)
A wrapper over a `LoopbackTransport` endpoint that holds each sent message for D "ticks" before
delivering it (a small queue keyed by a release counter the test advances), deterministically forcing
predictions to be stale → rollbacks. Lives in the test TU (not shipped in the lib).

### `refcore_rollback` core (`cores/refcore_rollback/`)
A minimal **driven** core whose state depends on input: an accumulator `acc` that each `run_frame` does
`acc += input.keys['X'] ? 2 : 1`, and renders `acc` into the framebuffer (so a diverged state is
visible/byte-different). `serialize_size` = `sizeof(uint32_t)`; serialize/unserialize the accumulator.
Reads **port 0** input via the v5 `input_state(host, 0, …)` callback. This makes a mispredicted input
genuinely change state, so rollback correction is really exercised. It does **not** touch the existing
`refcore_driven` (whose tests still assert +1/frame).

## 2. Data flow — one rollback tick

Both machines run the identical loop. At local frame F (with `out_rgba` for the displayed frame):
1. **Local input:** `local_inputs_[F] = L`; send `Input{F, local_port, L}`.
2. **Drain transport:** for each `Input{G, R}` → `remote_inputs_[G] = R`, `confirmed_ = max(confirmed_, G)`;
   for each `Checksum{G, crc}` → stash `peer_crc_[G]`. Timeout → `Stalled`; transport error → `Disconnected`.
3. **Rollback if mispredicted:** `g = first_mispredicted(verified_+1, min(confirmed_, F-1))`. If found:
   `load_state(state_ring_[g])`; for `f = g … F-1`: set `input[local_port]=local_inputs_[f]`,
   `input[remote_port]=(remote_inputs_ has f ? real : predict_remote(f))` (and record it into
   `predicted_inputs_[f]`), `advance(emit_audio=0)`, `save_state(state_ring_[f+1])`. `rollback_count_++`.
   Set `verified_ = min(confirmed_, F-1)`.
4. **Stall guard:** if `F - confirmed_ > max_prediction_` → return `Stalled` (don't simulate F yet; the
   ring won't grow unbounded). The caller re-ticks; draining continues.
5. **Simulate + display F:** `save_state(state_ring_[F])` (pre-frame); set `input[local_port]=L`,
   `input[remote_port]=predict_remote(F)` (record into `predicted_inputs_[F]`); `advance(emit_audio=1)`;
   `render(out_rgba)`. `frame_ = ++F`.
6. **Desync (confirmed frames only):** every K=60 confirmed frames, `crc = crc32(state at a confirmed
   frame)`; send `Checksum{that_frame, crc}`; compare to the peer's stashed crc for the same frame →
   mismatch = `Desync`. Never checksum a predicted (unconfirmed) frame. Prune `state_ring_`/history
   below `verified_ - slack`.

**Initial sync** (before the loop): host `serialize()`→`STATE_SYNC`→client `load_state` (Slice G), so
frame `start` begins byte-identical.

## 3. Why confirmed states converge

At any confirmed frame g, both machines have the **same** local+remote inputs (each side's local is
authoritative and was sent; the remote is the real received input). Both re-simulate g from the same
saved pre-state with those same inputs on a deterministic core → identical confirmed state, provably
equal to a lockstep run of that input sequence. Predicted (unconfirmed) frames may differ between the
machines transiently, but every frame becomes confirmed within `max_prediction` and is corrected. The
portable gate asserts exactly this: confirmed states == a no-latency ground-truth run.

## 4. Error handling

- **Prediction window exceeded** → `Stalled` (bounded; caller re-ticks). Not a crash, ring bounded.
- **Transport failure / peer close** → `Disconnected`; runtime holds last state; no crash.
- **Desync (confirmed-frame checksum mismatch)** → `Desync` with the frame#; halt+report. This is what
  turns a non-deterministic core or an incomplete savestate into a reported failure (rollback amplifies
  any nondeterminism, so this guard matters more than in lockstep).
- **save/load failure mid-resim** → surfaced (`Desync`/error), never crash.
- **Audio on rollback** → resim frames are **muted** (`emit_audio=0`); the live frame plays. A large
  correction yields a brief audio glitch and a small visual pop — **accepted** this slice (audio/visual
  rollback smoothing deferred).
- **Crash honesty** unchanged (in-process; a bad core can fault its host).

## 5. Testing

- **Portable rollback gate (input-sensitive `refcore_rollback`, device-free):** pick a fixed 2-port
  input sequence. (a) **Ground truth:** run a no-delay session (or a direct lockstep) recording the
  confirmed state at each frame. (b) **Rollback run:** two `RollbackSession`s over `DelayTransport`
  (remote inputs delivered D≥2 frames late) driven by the same input sequence. Assert **`rollback_count
  > 0`** (mispredictions really occurred) AND every **confirmed** frame's serialized state equals the
  ground truth, byte-exact. Proves resim *corrects* real divergence, no device.
- **Gated FCEUmm rollback gate (real NES):** two Donkey Kong runtimes over `DelayTransport`, differing
  per-port inputs, one side delayed → assert `rollback_count > 0` and both converge to identical
  confirmed states (crc-equal) over ~120 frames. `WARN`-skips if core/ROM absent.
- **Units:** `rb_predict` (repeat-last / neutral); state-ring bounded push/prune; `first_mispredicted`
  (earliest differing confirmed frame; sentinel when none); the **advance/render split** — `advance`
  runs the core but does **not** composite (framebuffer retained), `render` composites the last frame,
  and `advance(emit_audio=0)` submits **no** audio (via `rp_runtime_audio_stats` staying flat) while
  `advance(1)` does.
- **Harness:** a `--rollback` modifier on the Slice G netplay flags (`--netplay-host`/`--netplay-join`)
  selects `RollbackSession`; real 2-machine LAN play is **manual/deferred** (user-verified-pending).
- **Regression:** the A–G suite stays green. `present()` == `advance(1)+render` keeps single-player /
  lockstep byte-identical; the new core and split are additive.

## 6. Scope

**In Slice H:** the `advance`/`render` split + audio suppression (additive C API, no core-ABI change);
`RollbackSession` (predict / state-ring / rollback-resim / stall / confirmed-frame desync);
`refcore_rollback` input-sensitive test core; `DelayTransport` (test); portable + gated rollback gates
+ the unit tests; harness `--rollback`.

**Explicitly out (deferred):** UDP transport; input-delay + rollback hybrid tuning; audio/visual
rollback smoothing (accepted glitch/pop this slice); >2 players; presenting-core netplay; matchmaking /
relay / NAT / reconnect / spectators; the standing list (more validated systems, cross-API interop,
wrapping real heavy apps, out-of-process isolation, iOS/Android, EverythingBox integration).

**The single provable claim of Slice H:** *Two RetroPark runtimes run a deterministic driven core with
predicted remote input, and a mispredicted input is corrected by loading a savestate and re-simulating
silently — proven byte-exact against a lockstep ground truth on every confirmed frame, with rollbacks
provably occurring, using an input-sensitive reference core (portable) and real FCEUmm/Donkey Kong
(gated), reusing the Slice G transport. No UDP, one vs one, driven cores only.*

## 7. Repo additions

```
include/retropark/retropark.h        # + rp_runtime_advance / rp_runtime_render (additive)
src/runtime/Runtime.h/.cpp           # advance/render split, suppress_audio_ gate in on_audio_sample
src/net/
  RollbackSession.h/.cpp             # predict + state-ring + rollback-resim + stall + confirmed desync
  RollbackPredict.h                  # rb_predict + first_mispredicted (pure, tested)
cores/refcore_rollback/…             # tiny input-sensitive driven core (acc += button?2:1)
harness/windowed/main.cpp            # --rollback modifier -> RollbackSession
tests/
  test_rollback_unit.cpp             # rb_predict / state-ring / first_mispredicted / advance-render split
  test_rollback_e2e.cpp             # portable convergence gate + gated FCEUmm rollback gate (+ DelayTransport)
```
