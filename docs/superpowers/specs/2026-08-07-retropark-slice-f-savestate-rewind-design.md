# RetroPark — Slice F Design (savestate + rewind)

**Date:** 2026-08-07
**Status:** Approved (design), pending implementation plan
**Scope:** Serialize/restore a driven core's state (savestate) and a bounded per-frame ring for
frame-by-frame rewind — wiring the already-declared serialize hooks + an additive C API. No
core-ABI change.

---

## 0. Context and goal

The core ABI declared savestate hooks in Slice C (stubbed): `rp_core_abi.serialize_size`,
`serialize`, `unserialize`. FCEUmm exports `retro_serialize_size`/`retro_serialize`/
`retro_unserialize` (verified). This slice fills those hooks in and adds a runtime/C-API layer for
**savestate** (capture/restore state to a buffer) and **rewind** (a bounded ring of recent
snapshots the game steps backward through).

Host-owns-the-state is exactly what the **driven** model was built for, so savestate/rewind target
**driven cores** (libretro). Presenting/heavy-app savestate (they have their own) stays deferred.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Scope | **Savestate + rewind, driven cores only.** Presenting-core savestate deferred. |
| API | **Buffer-based** `save_state`/`load_state` (query `serialize_size` first). Files/slots are the caller's job. |
| Rewind model | **Uncompressed per-frame bounded ring** (drop oldest when full). Delta-compression is a transparent later win behind the same API. |
| Reach | Wire serialize on **both** the libretro shim (→ `retro_*`, real NES proof) and `refcore_driven` (its frame counter → a portable, device-free deterministic test). |
| ABI | **No core-ABI change** (serialize hooks already exist; ABI stays v4). Only five additive `rp_runtime_*` C-API functions. |

---

## 1. Components (no core-ABI change)

**Fill in the serialize hooks (were null):**
- `LibretroShim`: `sh_serialize_size` → `retro_serialize_size()`; `sh_serialize(data,size)` →
  `retro_serialize(data,size)` (`RP_ERR_UNSUPPORTED` if it returns false); `sh_unserialize(data,size)`
  → `retro_unserialize(data,size)`. Resolve the `retro_serialize`/`retro_unserialize` pointers in
  `create` and put the three fns in `kAbi`.
- `refcore_driven`: its state is the animation frame counter (a `uint32_t`). `serialize_size` → 4;
  `serialize` writes the counter; `unserialize` reads it. Restoring the counter makes the next
  `run_frame` render the identical frame — a portable, deterministic savestate.

**`CoreLoader`** passthroughs (guard state Created/Started; `RP_ERR_UNSUPPORTED` / size 0 when the
core's fn is null):
```cpp
size_t    serialize_size();
rp_result serialize(void* data, size_t size, std::string& err);
rp_result unserialize(const void* data, size_t size, std::string& err);
```

**Runtime + additive C API** (`retropark.h`):
```c
size_t    rp_runtime_serialize_size(rp_runtime* rt);
rp_result rp_runtime_save_state(rp_runtime* rt, void* buf, size_t size);
rp_result rp_runtime_load_state(rp_runtime* rt, const void* buf, size_t size);
rp_result rp_runtime_set_rewind(rp_runtime* rt, int enabled, uint32_t max_snapshots);
rp_result rp_runtime_rewind(rp_runtime* rt);
```
- `serialize_size` → `loader_.serialize_size()` (0 if no core / unsupported).
- `save_state(buf,size)` → require a driven core with serialize; `sz = serialize_size()`; `sz==0` →
  `RP_ERR_UNSUPPORTED`; `size < sz` → `RP_ERR_BAD_ARG`; else `loader_.serialize(buf, sz)`.
- `load_state(buf,size)` → `loader_.unserialize(buf, size)` (the core rejecting an incompatible
  state → `RP_ERR_UNSUPPORTED`, surfaced not crashed).
- `set_rewind(enabled, max_snapshots)` → require a serialize-capable driven core (`RP_ERR_UNSUPPORTED`
  else); set `rewind_enabled_`, `rewind_max_` (a sane default if 0), clear the ring.
- `rewind` → see §2.

## 2. Save / load / rewind flow

**Savestate:** `serialize_size` → alloc buffer → `save_state(buf)` serializes the core into it;
`load_state(buf)` restores it. A no-serialize core → `RP_ERR_UNSUPPORTED`.

**Rewind ring:** with rewind enabled, each **forward** `present()` (driven path) — at the top,
*before* `run_frame` — serializes the core's pre-frame state into a bounded ring (`std::deque`;
`pop_front` the oldest when the count exceeds `rewind_max_`). Then it runs the frame + composites as
usual. So the ring holds the pre-frame states of the last `rewind_max_` frames.

`rewind(rt)`:
- If rewind is disabled → `RP_ERR_INTERNAL`; if the ring can't step back → `RP_ERR_NOT_FOUND`
  (no history left).
- Otherwise it steps the game **one frame into the past**: pop the newest snapshot (the pre-state of
  the just-displayed frame), restore the now-newest snapshot (the previous frame's pre-state), and
  re-run that single frame so the composited output is the previous frame. Repeated `rewind()` calls
  walk backward frame-by-frame until the ring empties. Resuming forward `present()` (no rewind)
  continues capturing from the restored point.

Memory is bounded (`rewind_max_ × serialize_size`; NES state is a few KB). `serialize_size` is
assumed stable after load (true for NES/most cores); a mid-session change is a deferred edge.

## 3. No core-ABI change

`RETROPARK_ABI_VERSION` stays **4**. The `serialize_size`/`serialize`/`unserialize` fields have
existed on `rp_core_abi` since Slice C — this slice implements them and adds five *additive*
`rp_runtime_*` C-API functions. The rewind ring lives entirely in the runtime (the host owning the
state is the driven model's whole premise).

## 4. Error handling

- **No serialize support** (null hooks / presenting core / no core) → `save_state`/`load_state`/
  `serialize_size`/`set_rewind`/`rewind` → `RP_ERR_UNSUPPORTED` (or `RP_ERR_INTERNAL`); never a crash.
- **`save_state` buffer `< serialize_size`** → `RP_ERR_BAD_ARG`. **`load_state` incompatible state**
  (wrong ROM/core — `retro_unserialize` returns false) → `RP_ERR_UNSUPPORTED`, surfaced.
- **`rewind` with no history** → `RP_ERR_NOT_FOUND`; **`rewind` when disabled** → `RP_ERR_INTERNAL`.
  Neither crashes.
- **Bounded memory**; a serialize failure mid-capture skips that snapshot. `serialize_size` change
  mid-session is deferred (assumed stable).
- **A/V on a jump:** `load_state`/`rewind` makes video jump (next composite shows the restored frame)
  and audio glitch — **audio is not rewound** this slice. All on the host/present thread.
- **Crash honesty** unchanged (in-process; a bad core can still fault the host).

## 5. Testing

- **Portable savestate e2e (`refcore_driven`, deterministic, no device):** run to a state →
  `serialize_size` (== 4) → `save_state(buf)` → `present` once → readback R1 → advance N frames
  (frame changes) → `load_state(buf)` → `present` once → readback R2 → **assert R1 == R2** (counter
  restored → identical re-rendered frame). Proves the whole path with no dependency.
- **Gated FCEUmm savestate e2e (real NES, deterministic):** advance past boot → `save_state` →
  `present` → R1 → advance ~60 frames (assert R1 changed) → `load_state` → `present` → R2 →
  **assert R1 == R2** (NES re-executes deterministically from the restored state). `WARN`-skips if
  core/ROM absent.
- **Rewind e2e (`refcore_driven`, portable):** `set_rewind(on, N)` → run M forward frames
  (capturing) → `rewind()` K times (present between) → **assert the displayed frame stepped back
  ~K** (the counter decreased monotonically); `rewind()` on an empty ring → `RP_ERR_NOT_FOUND`, no
  crash. Plus a small ring-bookkeeping unit (push / drop-oldest / bounds).
- **Harness (human proof):** F5 save / F7 load / hold a key = rewind — watch Donkey Kong run
  **backward** then restore. The automated e2e is the gate.
- **Regression:** the A–E suite stays green — the serialize hooks just fill existing nulls; the
  save/rewind is new C API; presenting and existing driven paths are unaffected (`refcore_driven`
  now serializes, but its existing e2e is unchanged).

## 6. Scope

**In Slice F:** implement `serialize_size`/`serialize`/`unserialize` on `refcore_driven` (counter) +
the libretro shim (`retro_*`); `CoreLoader` serialize passthroughs; runtime + C API
(`serialize_size`, `save_state`, `load_state`, `set_rewind`, `rewind`) with a per-frame bounded
uncompressed ring captured in `present()`; portable savestate e2e + gated FCEUmm savestate e2e +
rewind e2e + a ring unit test + harness save/load/rewind keys.

**Explicitly out (deferred):** presenting-core savestate; delta-compression for the ring; rewinding
audio; savestate files/slots/versioning (buffer primitive only); netplay (savestate is its
foundation, its own slice); `serialize_size` changing mid-session; the standing list (more validated
systems, cross-API interop, wrapping real heavy apps, out-of-process isolation, iOS/Android,
EverythingBox integration).

**The single provable claim of Slice F:** *a driven core's state serializes and restores
**deterministically** (save at A → advance → load → provably back at A, pixel-exact), and a bounded
per-frame ring enables frame-by-frame rewind — proven with the reference driven core (portable) and
FCEUmm (real NES), and shown by rewinding the game in the harness. No core-ABI change.*

## 7. Repo additions

```
include/retropark/retropark.h        # + serialize_size/save_state/load_state/set_rewind/rewind
src/loader/CoreLoader.h/.cpp          # serialize/unserialize/serialize_size passthroughs
src/runtime/Runtime.h/.cpp            # save/load, rewind ring, per-frame capture in present()
cores/refcore_driven/RefCoreDriven.cpp   # serialize the frame counter
cores/libretro_shim/LibretroShim.cpp     # serialize -> retro_serialize*/unserialize
harness/windowed/main.cpp             # F5 save / F7 load / rewind key
tests/
  test_savestate.cpp                  # portable savestate + gated FCEUmm savestate + rewind + ring unit
```
