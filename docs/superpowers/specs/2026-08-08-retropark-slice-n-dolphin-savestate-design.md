# RetroPark — Slice N Design (Dolphin savestates)

**Date:** 2026-08-08
**Status:** Approved (design)
**Scope:** Wire `dolphin_present`'s serialize hooks to Dolphin's native in-memory savestate, so
`rp_runtime_save_state`/`rp_runtime_load_state` capture and restore a running GameCube game through the
core ABI. The foundation for Dolphin rewind/netplay later. **No core-ABI change** (the serialize hooks
have existed since Slice F; ABI stays v5).

---

## 0. Context and goal

Slices I–M made `dolphin_present` render, play audio, and take input through RetroPark's Runtime — but
it has **no savestate** (Slices K/L/M all deferred it). RetroPark already has the machinery: the ABI's
`serialize_size`/`serialize`/`unserialize` hooks (Slice F), and `Runtime::save_state`/`load_state` +
`rp_runtime_save_state`/`load_state` — which, crucially, are **NOT gated on driven cores**: `save_state`
only requires `serialize_size() > 0`, and `load_state` just calls `unserialize`. So a presenting core
that implements the serialize hooks gets host save/load for free — the Runtime needs no change.

Dolphin already has in-memory savestate internally: `State::SaveToBuffer(system, UniqueBuffer<u8>&)`
(grows the buffer to fit, returns the size) and `State::LoadFromBuffer(system, std::span<u8>)` — but
both are `static` in `State.cpp`. Savestates must run with the CPU quiesced; Dolphin provides
`Core::RunOnCPUThread(system, fn)` to run a function synchronously on the CPU thread. Slice N exposes the
two buffer functions and drives them from the vehicle's serialize hooks.

### Decisions

| Decision | Choice |
|---|---|
| Mechanism | **In-memory** via Dolphin's existing `SaveToBuffer`/`LoadFromBuffer` (exposed by a small `State.h`/`.cpp` patch), not temp files. No disk, no compression-format handling. |
| Threading | The public wrappers run the save/load **on the CPU thread** via `Core::RunOnCPUThread` (matching Dolphin's own `State::SaveAs`/`Load`) and **wait for completion** with a promise/future — `RunOnCPUThread` is fire-and-forget, so the wait is what makes our serialize synchronous and the snapshot consistent. |
| Size protocol | `dp_serialize_size` performs the save into an internal `g_state_buf` and returns its size; `dp_serialize` memcpys that captured buffer. This is an **atomic snapshot** taken at the size call (the Runtime always calls `serialize_size()` immediately before `serialize()`). |
| Runtime | **Unchanged** — `save_state`/`load_state` already work for any serialize-capable core. |
| ABI | **No change** (serialize hooks exist since Slice F; ABI stays v5). |

---

## 1. Components

### Dolphin — expose in-memory savestate (`Core/State.h` + `Core/State.cpp`, patch)
- Add two **public** wrappers (declared in `State.h`, defined in `State.cpp`) that call the existing
  `static` `SaveToBuffer`/`LoadFromBuffer`, marshalled onto the CPU thread:
  - `std::size_t SaveToBuffer(Core::System& system, Common::UniqueBuffer<u8>& buffer);` — runs the save on
    the CPU thread via `RunOnCPUThread`, returns the required size (0 on failure).
  - `bool LoadFromBuffer(Core::System& system, std::span<u8> buffer);` — runs the load on the CPU thread,
    returns success.
  - (If the existing statics collide by name, the public wrappers get distinct names, e.g.
    `SaveToBufferSync`/`LoadFromBufferSync`; the plan picks the exact names after reading `State.cpp`.)
- The wrappers must **wait** for the CPU-thread work to complete before returning (synchronous), so the
  vehicle's serialize call returns a fully-captured buffer.

### Vehicle — wire the serialize hooks (`external/dolphin/.../rp_dolphin.cpp`, patch)
- An internal `Common::UniqueBuffer<u8> g_state_buf` (or `std::vector<u8>`) holds the last captured state.
- `dp_serialize_size(core)`: if not booted → return 0. Else `size = State::SaveToBufferSync(system,
  g_state_buf)`; return `size`.
- `dp_serialize(core, void* data, size_t size)`: if `size < g_state_buf.size()` or no capture →
  `RP_ERR_BAD_ARG`; else memcpy `g_state_buf` (its captured bytes) into `data`; return `RP_OK`.
- `dp_unserialize(core, const void* data, size_t size)`: if not booted → error; copy `data`/`size` into a
  local buffer and `State::LoadFromBufferSync(system, {ptr, size})`; return `RP_OK`/error.
- Wire `dp_serialize_size`/`dp_serialize`/`dp_unserialize` into the `kAbi` vtable's currently-null
  serialize slots (`serialize_size`, `serialize`, `unserialize`). No other ABI slots change.
- Robustness: all three no-op/error cleanly before boot or if `Core::System`'s state isn't running; a
  failed save/load is surfaced as an error code, never a crash. After a load the video producer + audio
  puller keep running (they render/play the restored state; a brief audio glitch on the jump is accepted).

### RetroPark Runtime + C API — unchanged
`Runtime::save_state`/`load_state`/`serialize_size` and `rp_runtime_save_state`/`load_state`/
`serialize_size` already exist and are core-type-agnostic (gated only on `serialize_size() > 0`). Nothing
to change; dolphin_present reporting a non-zero size lights them up.

### Harness — load Dolphin, so F5/F7 apply (`harness/windowed/main.cpp`)
- The F5=save / F7=load key handler is **already generic** (`rp_runtime_serialize_size` +
  `rp_runtime_save_state`/`load_state`, not driven-gated). The gap is that the harness can only load the
  built-in refcores (chosen by `--driven`/default) — there's no way to load `dolphin_present`. Add a
  minimal **`--core <dir>`** flag: when present, `rp_runtime_load_core(<dir>)` that directory (a Vulkan
  presenting core) and `--content <iso>` feeds it the ROM — so the harness can run Dolphin, and F5/F7 then
  save/load it unchanged. This also lets the user hand-verify Dolphin input/audio (the Slice-M residual).
  Human proof: `--api vulkan --core <dolphin dir> --content <iso>`, play, F5 save, play on, F7 restore.

## 2. Data flow

```
save:  rp_runtime_save_state(buf,size) -> Runtime::save_state
         -> loader_.serialize_size() -> dp_serialize_size: RunOnCPUThread(SaveToBuffer -> g_state_buf); return size
         -> loader_.serialize(buf,size) -> dp_serialize: memcpy g_state_buf -> buf     (snapshot from the size call)
load:  rp_runtime_load_state(buf,size) -> Runtime::load_state
         -> loader_.unserialize(buf,size) -> dp_unserialize: RunOnCPUThread(LoadFromBuffer({buf,size}))
```

## 3. Error handling / robustness

- Not booted / no running game → `dp_serialize_size` returns 0 (`Runtime::save_state` → `RP_ERR_UNSUPPORTED`);
  `dp_unserialize` returns an error. Never a crash.
- Undersized caller buffer → `dp_serialize` returns `RP_ERR_BAD_ARG` (the Runtime also checks `size < sz`).
- A Dolphin save/load failure (bad buffer, state mismatch) is surfaced as `RP_ERR_INTERNAL`, not a crash.
- Cross-build/cross-session portability is **out of scope**: Dolphin savestates are build- and settings-
  specific. Within one session (same DLL) save→load is valid; persisting a buffer across app runs is the
  caller's risk and not guaranteed. Note this in the harness/help, don't enforce it here.

## 4. Testing

- **Round-trip determinism (gated real-GPU, the core proof):** in a **single boot**, boot Billy Hatcher
  through the Runtime → advance to a settled frame N → `rp_runtime_save_state` (size via
  `rp_runtime_serialize_size`) → advance K more frames, capture readback `A` at N+K → `rp_runtime_load_state`
  (restore N) → advance K again, capture readback `B` → assert `A == B` AND that the state is non-trivial
  (`A` differs from the frame at N — the game actually advanced K frames, so the match isn't a frozen
  screen). **Hold input constant (neutral, `set_input(0, {})`) across both K-advances** so the
  re-simulation is deterministic. Target byte-identical; if Dolphin's GPU path shows minor nondeterminism,
  the implementer may fall back to a tight tolerance (near-identical, e.g. < 0.1% of bytes differ) and
  report it — the intent is "the restored state re-runs to the same frame." Device-independent readback
  comparison; opt-in `RP_RUN_DOLPHIN=1`, WARN-skips without GPU/DLL/ROM. New
  `tests/test_dolphin_savestate_e2e.cpp` (its own gated case — cleaner than folding into the existing e2e).
- **Regression:** full A–M suite green; the driven savestate/rewind tests (Slice F) still pass; the
  Slice-J direct-C-API mode is unaffected (serialize hooks are only exercised via the Runtime path).

## 5. Scope

**In Slice N:** expose Dolphin's in-memory `SaveToBuffer`/`LoadFromBuffer` (public, CPU-thread-synced);
wire `dolphin_present`'s `serialize_size`/`serialize`/`unserialize`; harness F5/F7 for Dolphin; the gated
round-trip determinism e2e. **No ABI change.**

**Out (later):** Dolphin **rewind** (a per-frame GC savestate ring is heavy — its own slice), **netplay-
for-Dolphin**, savestate **files/slots/versioning/thumbnails**, cross-build/session portability, the
**input-effect test** (needs a hand-navigated interactive savestate — deferred as fragile), delta/
compressed state, rewinding audio.

**The single provable claim:** *`rp_runtime_save_state`/`load_state` capture and restore a running Dolphin
GameCube game through `rp_core_abi` — save, diverge, restore, and the emulation re-runs deterministically
to the same frame — no libretro, proven by a gated round-trip determinism e2e.*

## 6. Repo additions

```
external/dolphin/Source/Core/Core/State.h, State.cpp     # public CPU-thread-synced SaveToBuffer/LoadFromBuffer (patch)
external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp # dp_serialize_size/serialize/unserialize + kAbi wiring (patch)
harness/windowed/main.cpp                                # F5/F7 save/load for the Dolphin (presenting) core
tests/test_dolphin_savestate_e2e.cpp                     # gated round-trip determinism e2e
docs/patches/dolphin-external-present.patch              # refreshed with the savestate changes
```
