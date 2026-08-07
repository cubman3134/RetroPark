# RetroPark — Slice G Design (netplay: delay-based lockstep, 1v1, LAN-direct)

**Date:** 2026-08-07
**Status:** Approved (design), pending implementation plan
**Scope:** Two RetroPark runtimes, each owning one player, exchange per-frame input over a transport
and advance a **deterministic driven core** in **delay-based lockstep** from a **savestate-synced**
initial state. Savestate (Slice F) is the sync primitive; determinism is the invariant; the transport
just moves inputs. One-vs-one, LAN direct-connect, driven cores only.

---

## 0. Context and goal

Slice F shipped savestate/rewind — and the spec called out that **savestate is netplay's
foundation**. This slice builds on it: netplay keeps two machines' driven cores in perfect sync by
(1) starting both from the *same* serialized state and (2) feeding both the *same* per-frame inputs,
relying on the core's determinism (same inputs → same state) so their states never diverge.

The input path already exists but is **single-player**: one `rp_input_state` (`retropark_abi.h`),
set via `rp_runtime_set_input`, pulled by the core each frame through the `input_state` host
callback (the libretro shim maps it to libretro's per-port `input_state_cb`). Netplay needs **two
ports** live at once (P1 + P2 pressing different buttons the same frame), each machine owning one and
receiving the other over the wire.

### Decisions already made (this brainstorm)

| Decision | Choice |
|---|---|
| Model | **Delay-based lockstep.** Both sides exchange inputs and advance only when both are in; a small input delay hides latency. Rollback/prediction is the *next* netplay slice, built on this. |
| Transport | **Direct TCP, host + connect** (no matchmaking/relay), behind an **`ITransport`** seam (Loopback impl for the deterministic gate, Tcp impl for the wire). |
| Input model | **Two fixed ports** (local + remote). Runtime holds `input_[2]`; each machine sets its local port from the harness and fills the remote port from the net each frame. |
| ABI | **Bump to v5.** The `input_state` host callback gains a `port` param (the one existing-signature change; the ABI is unpublished and all cores are in-repo). Everything else is additive C API. |
| Proof | **Determinism (loopback) + gated FCEUmm lockstep + localhost TCP round-trip + periodic desync CRC.** A real 2-machine LAN demo is manual/deferred (user-verified-pending). |

---

## 1. Components

### `ITransport` (`src/net/ITransport.h`)
A byte-frame channel — send/recv whole messages, not a raw stream (framing is the transport's job):
```cpp
struct ITransport {
    virtual ~ITransport() = default;
    // Send one complete message (framed). Returns RP_ERR_* on failure/disconnect.
    virtual rp_result send(const void* data, size_t size) = 0;
    // Receive one complete message into `out`. block=true waits (bounded by timeout_ms);
    // block=false polls (RP_ERR_NOT_FOUND if nothing ready). Disconnect -> RP_ERR_*.
    virtual rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) = 0;
    virtual bool connected() const = 0;
    virtual void close() = 0;
};
```

**`TcpTransport` (`src/net/TcpTransport.{h,cpp}`)** — Winsock (`ws2_32`).
- `host(port)`: `WSAStartup` → socket → bind → listen → **accept exactly one** peer, then the
  listener closes. `join(ip, port)`: socket → connect.
- Framing: each message is `uint32 LE length` + payload; `recv` loops until a full message is read
  (handles TCP partial reads). Blocking recv uses `SO_RCVTIMEO` for the bounded wait.
- `TCP_NODELAY` set (lockstep is latency-sensitive; Nagle would batch tiny input frames).

**`LoopbackTransport` (`src/net/LoopbackTransport.{h,cpp}`)** — two in-process endpoints sharing two
queues (A→B, B→A), mutex + condition-variable guarded so two runtimes can run on two threads or be
pumped alternately on one. `make_loopback_pair() -> {a, b}`. This is what makes the determinism gate
device-free and deterministic.

### `NetProtocol` (`src/net/NetProtocol.{h,cpp}`)
Explicit **little-endian** wire messages (never `memcpy` a struct across the wire — pack/unpack field
by field so a big-endian or differently-padded peer stays compatible):
- `HELLO{ uint32 abi_version; char core_id[64]; uint64 content_hash; uint32 input_delay; uint64 start_frame; }`
  — exchanged both ways at connect; both sides must agree (else refuse).
- `STATE_SYNC{ uint64 frame; uint32 size; bytes blob; }` — host → client, the `serialize()` output.
- `INPUT{ uint64 frame; uint8 port; rp_input_state state; }` — one port's input for a given frame.
- `CHECKSUM{ uint64 frame; uint32 crc; }` — CRC32 of `serialize()` at `frame`, every K frames.
Each message: a 1-byte tag + packed payload. `net_encode_*` / `net_decode_*` helpers, unit-tested for
round-trip and endianness.

### `NetSession` (`src/net/NetSession.{h,cpp}`)
The orchestrator; drives one side of the session against a `Runtime&` and an `ITransport&`.
- `rp_result start_host(Runtime&, ITransport&, uint32_t input_delay, std::string& err)`:
  send/recv `HELLO` (validate abi/core/content match); host is authoritative for initial state →
  `serialize()` → send `STATE_SYNC`; assign **local port = 0**, remote = 1; set `start_frame`.
- `rp_result start_join(Runtime&, ITransport&, std::string& err)`: exchange `HELLO`; receive
  `STATE_SYNC` → `load_state`; **local port = 1**, remote = 0.
- `NetStatus tick(const rp_input_state& local_now)`: one lockstep frame (see §2). Returns
  `OK | WAITING | DESYNC | DISCONNECTED`.
- Owns the **input-delay ring**: `local_now` is the input entered *this* frame; it is queued and
  *applied* `input_delay` frames later, giving the wire that many frames of slack. A small pure
  helper (delay-ring push/fetch) is factored out and unit-tested.

### Runtime + ABI changes
- **Two-port input** (`src/runtime/Runtime.{h,cpp}`): `input_[2]` replaces the single `input_`.
  `rp_runtime_set_input(rt, uint32_t port, const rp_input_state*)` — **port arg added** (additive C
  API; port clamped to {0,1}). The single-player harness path just uses port 0.
- **ABI v5** (`include/retropark/retropark_abi.h`): `RETROPARK_ABI_VERSION` → 5; the host callback
  becomes `void (*input_state)(rp_host* host, uint32_t port, rp_input_state* out)`. The runtime's
  trampoline returns `input_[port]`. All in-repo cores recompile: refcore_driven / refcore_present /
  refcore_present_vk ignore the new param (they don't read input); the **shim** uses it to answer
  libretro's `input_state_cb(port, …)` from the matching runtime port.
- **CRC / serialize reuse**: `rp_runtime_serialize_size`/`save_state`/`load_state` (Slice F) are the
  state-sync + checksum primitives. A `crc32` helper (`src/net/Crc32.h`, table-based, pure, tested)
  computes the desync digest over the serialized buffer.

*(Considered and rejected: keeping ABI v4 and adding a second `input_state_port` callback alongside
the old one. Two ways to do one thing + permanent cruft; the signature change is cleaner and the ABI
is unpublished. Chosen: v5.)*

---

## 2. Data flow — one lockstep tick (input delay D)

Both machines run the **identical** loop; frame counter F starts at the agreed `start_frame`.
1. **Local input:** read local input `L` for frame F. Push it into the delay ring; the value actually
   *applied* this frame is `L_applied = ring.fetch(F)` (the input entered at F−D). Send
   `INPUT{ F, local_port, L }` (peer applies it at F+D on its side too).
2. **Remote input:** blocking-`recv` peer `INPUT` messages until the remote input `R` for frame **F**
   is in hand. (Out-of-order/early frames are buffered by frame#.) Timeout → `WAITING` then, if it
   persists, `DISCONNECTED`.
3. **Apply both:** `set_input(local_port, L_applied)`, `set_input(remote_port, R_for_F)`. Both
   machines now hold an **identical** `input_[2]`.
4. **Advance:** `rp_runtime_present()` runs the core one frame → identical resulting state
   (determinism).
5. **Desync guard:** every K frames (e.g. 60), compute `crc = crc32(serialize())`, send
   `CHECKSUM{F, crc}`, compare to the peer's `CHECKSUM{F, *}`; mismatch → `DESYNC` (with F).

The delay D lets the network deliver an input D frames before it's needed. For the loopback gate D can
be small (delivery is instant); the test drives a fixed input plan so both sides are fed identical
sequences and we assert **state-equality every frame**.

**Initial sync (before the loop):** host `serialize()` → `STATE_SYNC` → client `load_state()`, so
frame `start_frame` begins from a byte-identical state on both sides.

---

## 3. Error handling

- **Disconnect / recv failure / peer close** → `tick` returns `DISCONNECTED`; the runtime keeps its
  last state; the harness surfaces it; never a crash.
- **Desync (checksum mismatch)** → `DESYNC` with the frame#; netplay halts (continuing is unsafe) and
  reports. This is the honest divergence signal — it's exactly what catches a non-deterministic core
  or an incomplete savestate.
- **Handshake mismatch** (abi/core-id/content-hash differ) → refuse to start with a clear error; no
  session.
- **Partial / large messages** → length-prefixed framing loops until complete; a serialize blob is a
  few KB (well within one `STATE_SYNC`).
- **Blocking wait for remote input** → bounded by `timeout_ms`; on timeout `WAITING` (harness shows
  "waiting for peer"), escalating to `DISCONNECTED`.
- **Non-deterministic core** → out of scope; **not silently tolerated** — the periodic checksum turns
  its drift into a reported `DESYNC`. Only FCEUmm (deterministic) is validated.
- **A/V on the far side** unchanged (each machine renders + plays its own audio locally; no A/V is
  sent over the wire — only inputs + the initial state + checksums).
- **Crash honesty** unchanged (in-process; a bad core can still fault its host).

---

## 4. No silent single-player regression

The two-port change must not break existing single-player. `rp_runtime_set_input(rt, 0, in)` is the
old behavior; the windowed harness's normal (non-netplay) path sets port 0 only, and port 1 stays
zeroed. The ABI v5 recompile touches every core but only the shim reads the new `port` param.

---

## 5. Testing

- **Gate 1 — loopback determinism (portable, `refcore_driven`):** two `Runtime`s + a
  `LoopbackTransport` pair, a scripted 2-port input plan, run **300 frames**, assert
  `serialize(A) == serialize(B)` **every frame** and that neither side reports `DESYNC`. The core
  netplay guarantee, device-free. *(refcore_driven ignores input, so to make inputs actually move
  state for a sharper test, the plan may also assert the frame-counter states stay equal; determinism
  is proven by byte-equality regardless.)*
- **Gate 2 — gated FCEUmm lockstep (real NES):** two shim/FCEUmm runtimes + loopback, Donkey Kong,
  scripted differing per-port inputs, ~**120 frames**, assert identical serialized states in lockstep
  and no desync. `WARN`-skips if core/ROM absent (like the Slice D/F gated e2es).
- **Gate 3 — `TcpTransport` localhost round-trip:** `host(0.0.0.0:port)` + `join(127.0.0.1:port)` on
  two threads; send an `INPUT` and a `STATE_SYNC`-sized (multi-KB) message each way; assert the bytes
  received equal the bytes sent (framing + partial-read correctness). Proves the real wire path
  locally without two machines.
- **Units:** `NetProtocol` encode/decode round-trip for every message (endian-explicit); the
  input-delay ring (push/fetch/bounds); `crc32` against known vectors.
- **Harness — 2-machine LAN demo:** `--netplay-host <port>` / `--netplay-join <ip:port>`; two
  instances sync Donkey Kong from a savestate and play in lockstep, each driving its own port.
  **Manual, deferred** to real hardware (noted user-verified-pending). The automated gates are the
  merge gate.
- **Regression:** the A–F suite stays green. The ABI v5 recompile + two-port input are covered by the
  existing driven/e2e tests continuing to pass (single-player = port 0).

---

## 6. Scope

**In Slice G:** `ITransport` + `LoopbackTransport` + `TcpTransport`; `NetProtocol` (hello / state-sync
/ input / checksum, little-endian); `NetSession` (handshake, initial state sync, lockstep `tick`,
input-delay ring, desync CRC); Runtime two-port input + `rp_runtime_set_input(port, …)`; **ABI v5**
`input_state(port)` + shim per-port routing + refcore recompiles; `crc32` helper; Gates 1–3 + unit
tests + harness netplay flags.

**Explicitly out (deferred):**
- **Rollback / prediction** — the next netplay slice, built directly on this lockstep + fast
  save/load foundation.
- UDP transport; 3+ players / dynamic join-leave; matchmaking / relay / NAT traversal / internet
  play; spectators; reconnect / mid-session re-sync; input compression; A/V desync smoothing;
  presenting-core netplay.
- The standing list: more validated systems, cross-API interop, wrapping real heavy apps,
  out-of-process isolation, iOS/Android, EverythingBox integration.

**The single provable claim of Slice G:** *Two RetroPark runtimes, each owning one player, exchange
per-frame input over an `ITransport` and advance a deterministic driven core in delay-based lockstep
from a savestate-synced initial state — proven byte-identical (serialize-equal every frame, no
desync) in-process with the reference core and with real FCEUmm/Donkey Kong, with the real TCP wire
path exercised on localhost and a periodic state-checksum guarding divergence. No rollback, one vs
one, LAN direct.*

## 7. Repo additions

```
include/retropark/retropark_abi.h    # ABI v5: input_state gains uint32_t port
include/retropark/retropark.h        # rp_runtime_set_input gains a port arg
src/net/
  ITransport.h                       # transport interface
  LoopbackTransport.h/.cpp           # in-process paired-queue transport (tests)
  TcpTransport.h/.cpp                # Winsock host/join, length-prefixed framing
  NetProtocol.h/.cpp                 # hello/state-sync/input/checksum encode/decode (LE)
  NetSession.h/.cpp                  # handshake, state sync, lockstep tick, delay ring, desync
  Crc32.h                            # table-based CRC32 (pure)
src/runtime/Runtime.h/.cpp           # input_[2], two-port trampoline, set_input(port)
cores/libretro_shim/LibretroShim.cpp # route input_state(port) -> libretro input_state_cb(port,…)
cores/refcore_driven/…               # recompile for ABI v5 input_state(port) (ignores port)
cores/refcore_present*/…             # recompile for ABI v5 signature
harness/windowed/main.cpp            # --netplay-host / --netplay-join; drive local port, net the other
tests/
  test_net_protocol.cpp              # encode/decode round-trip + endianness + crc32 + delay ring
  test_netplay_e2e.cpp               # Gate 1 loopback determinism + Gate 2 gated FCEUmm lockstep
  test_net_tcp.cpp                   # Gate 3 localhost TcpTransport round-trip
```
