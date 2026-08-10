# RetroPark — Slice O Design (Dolphin netplay, drive Dolphin's built-in)

**Date:** 2026-08-08
**Status:** Approved (design)
**Scope:** Two machines play a GameCube game together through `dolphin_present`, using **Dolphin's own
built-in netplay** (delay-based lockstep + determinism mode). RetroPark hosts the shared surface, audio,
and local input; Dolphin runs the netplay session. First netplay slice for a presenting core — provable
on localhost with two processes; real cross-machine LAN is verification-pending. **No core-ABI change**
(control is a vehicle C API, like the Slice-J boot API).

---

## 0. Context and goal

Slices I–N made `dolphin_present` render, play audio, take input, and savestate through RetroPark. The
obvious netplay approaches that reuse RetroPark's own lockstep (Slice G/H) hit two walls for a presenting
core: (a) rollback needs a per-frame ~94 MB savestate (infeasible — Slice N), and (b) delay-lockstep
around Dolphin would require reimplementing Dolphin's cross-machine determinism, which Dolphin coordinates
inside its own netplay code — almost certain to desync.

Dolphin already has battle-tested netplay (`NetPlayServer`/`NetPlayClient` in `Source/Core/Core/`,
delay-based lockstep with a determinism mode — the same code used for real online GC/Wii play). Its UI
seam, `NetPlayUI`, is a **Core-level abstract interface** (no Qt dependency), so netplay is drivable
headless: implement a stub `NetPlayUI` and construct the server/client directly. Slice O drives Dolphin's
netplay and lets it handle determinism, state-sync, input-sync, lockstep, and desync detection; RetroPark
provides the window/audio/input it already does, and our Slice-M input override supplies the *local* pad
which Dolphin's netplay syncs across.

### Decisions

| Decision | Choice |
|---|---|
| Mechanism | **Drive Dolphin's built-in netplay** via a headless `NetPlayUI` stub + `NetPlayServer`/`NetPlayClient`. Not a RetroPark reimplementation. |
| Control | **Vehicle C API** (`rp_dolphin_netplay_host`/`_join`/`_status`), like the Slice-J `rp_dolphin_boot` API. Called before content boot. **No ABI change.** |
| Boot | When netplay is armed, the game boots via netplay's `StartGame → NetPlayUI::BootGame` (carrying the netplay `NetSettings`) with our producer/window/audio/input attached — not the direct `BootManager::BootCore` path. |
| Input | Unchanged Slice-M override supplies the **local** pad; Dolphin's netplay reads the local mapped controller, syncs it, and injects the combined stream. |
| Proof | **Localhost 2-process gate** (accepted); real cross-machine LAN verification-pending. |

---

## 1. Components

### Headless `NetPlayUI` stub (`external/dolphin/.../rp_dolphin.cpp`, patch)
Implement Dolphin's `NetPlayUI` (~35 pure-virtuals). Most are no-op/log stubs. Load-bearing:
- **`BootGame(path)`** — boot the ROM with the existing hidden-window + XFB producer + audio puller +
  input override setup, now carrying the netplay session (`BootSessionData` with the netplay `NetSettings`).
  This is the netplay-driven boot (the non-netplay path stays as-is).
- **`StopGame()`** — stop the running game.
- **`IsHosting()`** — true on the host.
- **`Update()`** — pump; may be a no-op if our host loop already dispatches.
- **`FindGameFile(sync_identifier, ...)`** — resolve the local ROM to a `UICommon::GameFile` matching the
  netplay `SyncIdentifier` (both peers must have the same game). Return the local Billy Hatcher `GameFile`.
- **`OnDesync(frame, player)` / `OnConnectionLost()` / `OnConnectionError()` / `OnPlayerConnect/Disconnect`
  / `OnMsgStartGame` / `OnMsgStopGame`** — set atomic flags/counters the vehicle exposes via
  `rp_dolphin_netplay_status` (connected, started, desynced, player_count, frame).

### Vehicle netplay control (C API — no ABI change)
`extern "C" __declspec(dllexport)`:
- `int rp_dolphin_netplay_host(uint16_t port)` — create `NetPlayServer(port, /*forward_port=*/false,
  &g_netplay_ui, ...)`; store it; returns 0 on success.
- `int rp_dolphin_netplay_join(const char* ip, uint16_t port)` — create `NetPlayClient(ip, port,
  &g_netplay_ui, name, ...)`; returns 0 on success/connected.
- `int rp_dolphin_netplay_start()` — host only: `ChangeGame(sync_id_of_rom)` + `SetupNetSettings()` +
  `RequestStartGame()` (the client boots automatically on `OnMsgStartGame`). (The ROM path is provided by
  the normal `load_content`/boot arming; the exact arming sequence is nailed in the plan.)
- `void rp_dolphin_netplay_status(int* connected, int* started, int* desynced, uint32_t* frame)` — the
  test/harness polls this.
- `void rp_dolphin_netplay_stop()` — tear down the session.

The session is armed *before* the game boots; `rp_dolphin_boot`/the Runtime `start` path checks a
"netplay armed" flag and routes boot through the netplay `StartGame` flow instead of a direct boot.

### RetroPark Runtime + ABI — unchanged
Netplay is core-owned and driven out-of-band via the vehicle C API; the Runtime keeps presenting Dolphin's
frames + audio + input exactly as in Slices K–N. No Runtime or ABI change.

### Harness — netplay flags (`harness/windowed/main.cpp`)
Reuse Slice-G-style flags for the Dolphin (`--core`) path: `--netplay-host <port>` and
`--netplay-join <ip:port>` call the vehicle's netplay C API (via `GetProcAddress` on the loaded core DLL)
before/around content load, so a human can play 2-player: host on one machine, join on the other.

## 2. Data flow

```
arm:   rp_dolphin_netplay_host(port)  [or _join(ip,port)]  -> NetPlayServer/Client + NetPlayUI stub
host:  (client connects) -> rp_dolphin_netplay_start() -> ChangeGame + SetupNetSettings + RequestStartGame
both:  OnMsgStartGame -> NetPlayUI::BootGame(rom) -> boot w/ NetSettings + producer/window/audio/input
play:  Dolphin netplay each frame: exchange inputs (delay ring), enforce determinism, detect desync
       our Slice-M override -> local pad -> Dolphin netplay syncs it -> both machines advance in lockstep
       RetroPark presents each machine's frames + audio locally (unchanged)
```

## 3. Error handling / robustness

- Connect/host failure (port in use, no peer, bad IP) → the C API returns non-zero; `rp_dolphin_netplay_status`
  reports not-connected; never a crash.
- Desync (Dolphin detects a state mismatch) → `OnDesync` sets a flag; the test asserts it does NOT fire in
  the happy path; a real desync is surfaced, not hidden.
- Connection lost mid-session → `OnConnectionLost` flag; the game keeps running locally (Dolphin's behavior),
  the status reports disconnected.
- In-process safety: netplay uses Dolphin's process-global state (one `NetPlayClient`/`Server` per process),
  so the 2-process gate is inherent — a single process cannot host two peers. Real play is 2 processes anyway.
- Boot-flow: if netplay arming fails, fall back to a normal single-player boot (or surface the error) — no
  half-initialized session.

## 4. Testing

- **Localhost 2-process gate (the automated proof, gated `RP_RUN_DOLPHIN=1`):** the test **spawns a second
  process** (re-exec the test binary with an env flag marking it the joiner, e.g. `RP_NETPLAY_ROLE=join`).
  Parent = host: `rp_dolphin_netplay_host(port)` on `127.0.0.1`; child = join:
  `rp_dolphin_netplay_join("127.0.0.1", port)`. Host waits for the player to connect, calls
  `rp_dolphin_netplay_start()`; both boot Billy Hatcher via Dolphin netplay; both run N frames (present +
  poll status). Assert on BOTH: `connected` and `started` become true, and `desynced` stays false across N
  frames (two Dolphins from the same synced state + same input stream staying in lockstep = netplay works).
  Child reports success via exit code; parent asserts child succeeded + its own status. WARN-skips without
  GPU/DLL/ROM. (Heavy: two real Dolphin boots — opt-in.)
- **Harness (human proof):** two machines, `--netplay-host`/`--netplay-join`, play 2-player. Real
  cross-machine LAN is **user-verified-pending** (can't drive two machines from one session — like Slice
  G/H, cast, EB netplay).
- **Regression:** full A–N suite green; single-player Dolphin (Slices K–N) and the driven-core netplay
  (Slice G/H) are untouched.

## 5. Scope

**In Slice O:** the headless `NetPlayUI` stub; the vehicle netplay C API (host/join/start/status/stop); the
netplay-driven boot fork; harness `--netplay-host`/`--netplay-join` for the Dolphin core; the localhost
2-process gate. **No ABI change.** Input composes with Slice M unchanged.

**Out (later):** real cross-machine LAN *verification*; 4-player; Wii/Wiimote netplay; traversal server /
NAT-punch-through / relay; spectators; host-input-authority / golf mode; Wii save-sync & SD; a RetroPark-
Runtime netplay API / ABI hook for presenting cores (core-owned via the vehicle C API for now); netplay
chat; reconnection.

**The single provable claim:** *Two `dolphin_present` instances play a GameCube game together through
Dolphin's own netplay — synchronized boot from a shared state, lockstep input exchange, determinism and
desync detection all handled by Dolphin, RetroPark hosting the window/audio/input — no libretro. Proven by
a gated 2-process localhost test (both connect, boot, and stay desync-free over N frames); real
cross-machine LAN pending.*

## 6. Repo additions

```
external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp  # NetPlayUI stub + netplay C API + boot fork (patch)
harness/windowed/main.cpp                                 # --netplay-host/--netplay-join for the --core path
tests/test_dolphin_netplay_e2e.cpp                        # gated 2-process localhost netplay gate
docs/patches/dolphin-external-present.patch               # refreshed with the netplay changes
```
