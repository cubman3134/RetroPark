# RetroPark Slice O — Dolphin Netplay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two `dolphin_present` instances play a GameCube game together via Dolphin's own built-in netplay (delay-lockstep + determinism), driven headless, proven by a gated 2-process localhost test.

**Architecture:** A headless `NetPlayUI` stub in the vehicle drives Dolphin's `NetPlayServer`/`NetPlayClient`; a vehicle C API (`rp_dolphin_netplay_host/join/start/status/stop`, no ABI change) arms the session before boot; when netplay starts, the game boots via `OnMsgStartGame → NetPlayClient::StartGame → NetPlayUI::BootGame` (carrying the netplay `BootSessionData`) with our existing window/producer/audio/input. The canonical reference for the driving sequence and the `NetPlayUI` method bodies is **`external/dolphin/Source/Core/DolphinQt/NetPlay/NetPlayDialog.cpp`** — mirror it, adapted headless.

**Tech Stack:** C++17, Dolphin (tag 2606) `NetPlayServer`/`NetPlayClient`/`NetPlayUI`/`UICommon::GameFile`, RetroPark vehicle + Runtime present path, doctest, Win32 `CreateProcess` (2-process test), MSBuild (Dolphin DLL) + CMake/MSBuild (RetroPark).

## Global Constraints

- **No AI attribution** in any commit message. Conventional prefixes (`feat:`/`fix:`/`docs:`).
- **ABI stays v5** — netplay control is a **vehicle C API** (`extern "C" __declspec(dllexport)`, like the Slice-J `rp_dolphin_boot`); do NOT change `rp_core_abi`.
- **`external/dolphin` is git-ignored** — Dolphin-side changes go to `docs/patches/dolphin-external-present.patch`. Regenerate stdout-only: `git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null`.
- **Never commit cores or ROMs.** Billy Hatcher ROM: `C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz`.
- **Dolphin DLL relink recipe:** build `DolphinLib.vcxproj` first if you touch any DolphinLib source, then `RetroParkDolphin.vcxproj` — both `-p:Configuration=Release -p:Platform=x64 -p:SolutionDir="…\external\dolphin\Source\\" -p:BuildProjectReferences=false -m -v:minimal -nologo` via **PowerShell**. (Slice O only edits `rp_dolphin.cpp`, which is in RetroParkDolphin — so normally only that project relinks; the glslang mkdir/copy error is a spurious flake, confirm via fresh DLL timestamp.) Use PowerShell for `dumpbin`. AfterBuild copies the DLL + `core.json` into `external/dolphin/Binary/x64/`.
- **RetroPark build:** `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug`. Full suite: `C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe` → `103 passed | 0 failed`. Dolphin e2e tests are opt-in `RP_RUN_DOLPHIN=1`, WARN-skip without GPU/DLL/ROM.
- **Reference to mirror:** `external/dolphin/Source/Core/DolphinQt/NetPlay/NetPlayDialog.cpp` (the Qt `NetPlayUI` impl + host start sequence) and `.../DolphinQt/Settings.cpp`'s NetPlayServer/Client ownership. The headless stub replicates the behavior without Qt.

---

### Task 1: Headless netplay session — CONNECT two peers (foundation)

Stand up a headless `NetPlayUI` stub and the vehicle C API, and prove two peers connect over loopback — WITHOUT booting a game yet. This de-risks the session plumbing + the 2-process test harness before the intricate boot flow.

**Files:**
- Modify: `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp` (NetPlayUI stub + `rp_dolphin_netplay_host/join/status/stop`)
- Create: `tests/test_dolphin_netplay_e2e.cpp` (gated 2-process CONNECT test)
- Modify: `tests/CMakeLists.txt` (register it)
- Modify: `docs/patches/dolphin-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: Dolphin `NetPlayServer(u16 port, bool forward_port, NetPlayUI* dialog, const NetTraversalConfig&)`, `NetPlayClient(const std::string& address, u16 port, NetPlayUI* dialog, std::string name, const NetTraversalConfig&)`, `NetPlayClient::IsConnected()`, `NetPlayServer::GetPlayers()`/player count; `NetPlay::NetTraversalConfig{}` (default = direct, no traversal); the abstract `NetPlayUI` (Core/NetPlayClient.h). Headers: `Core/NetPlayServer.h`, `Core/NetPlayClient.h`, `Core/NetPlayProto.h`.
- Produces: C exports `int rp_dolphin_netplay_host(uint16_t port)`, `int rp_dolphin_netplay_join(const char* ip, uint16_t port)`, `void rp_dolphin_netplay_status(int* connected, int* started, int* desynced, uint32_t* players)`, `void rp_dolphin_netplay_stop()`; a file-scope `RpNetPlayUI g_netplay_ui;` and `std::unique_ptr<NetPlay::NetPlayServer> g_netplay_server; std::unique_ptr<NetPlay::NetPlayClient> g_netplay_client;` plus atomic status flags. `rp_dolphin_netplay_start` is added in Task 2.

- [ ] **Step 1: Write the failing 2-process CONNECT test.** Create `tests/test_dolphin_netplay_e2e.cpp`. The test re-execs itself as the joiner via an env var; parent hosts, child joins; both assert connection. (The DLL is loaded and its netplay C API resolved via `LoadLibraryEx`+`GetProcAddress`, like the Slice-J handoff test.)

```cpp
#include <doctest/doctest.h>
#include "render/vulkan/VulkanBackend.h"
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace rp;
namespace {
const char* kDll = "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64/dolphin_present.dll";
bool file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

typedef int (*host_fn)(uint16_t);
typedef int (*join_fn)(const char*, uint16_t);
typedef void (*status_fn)(int*, int*, int*, uint32_t*);
typedef void (*stop_fn)();

struct NetApi { HMODULE dll; host_fn host; join_fn join; status_fn status; stop_fn stop; };
NetApi load_net() {
    HMODULE d = LoadLibraryExA(kDll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE(d);
    NetApi a{d,
        (host_fn)GetProcAddress(d, "rp_dolphin_netplay_host"),
        (join_fn)GetProcAddress(d, "rp_dolphin_netplay_join"),
        (status_fn)GetProcAddress(d, "rp_dolphin_netplay_status"),
        (stop_fn)GetProcAddress(d, "rp_dolphin_netplay_stop")};
    REQUIRE(a.host); REQUIRE(a.join); REQUIRE(a.status); REQUIRE(a.stop);
    return a;
}
const uint16_t kPort = 54700;
} // namespace

TEST_CASE("dolphin netplay: two processes connect over loopback (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping"); return; }
    if (!file_exists(kDll)) { WARN("dolphin_present.dll not built; skipping"); return; }

    if (std::getenv("RP_NETPLAY_ROLE")) {
        // Child = joiner. Connect to the host, wait to be connected, exit(0) on success / exit(2) on fail.
        NetApi a = load_net();
        int rc = a.join("127.0.0.1", kPort);
        int connected = 0, started = 0, desynced = 0; uint32_t players = 0;
        for (int i = 0; i < 100 && !connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            a.status(&connected, &started, &desynced, &players);
        }
        a.stop();
        std::exit((rc == 0 && connected) ? 0 : 2);
    }

    // Parent = host.
    NetApi a = load_net();
    REQUIRE(a.host(kPort) == 0);

    // Spawn ourselves as the joiner (same test-case filter + role env).
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" --test-case=\"dolphin netplay: two processes*\"";
    STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    SetEnvironmentVariableA("RP_NETPLAY_ROLE", "join");
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableA("RP_NETPLAY_ROLE", nullptr);
    REQUIRE(ok);

    int connected = 0, started = 0, desynced = 0; uint32_t players = 0;
    for (int i = 0; i < 100 && players < 2; ++i) {   // host sees itself + the joiner
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        a.status(&connected, &started, &desynced, &players);
    }
    CHECK(players >= 2);

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD child_rc = 1; GetExitCodeProcess(pi.hProcess, &child_rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    a.stop();
    CHECK(child_rc == 0);   // joiner connected
}
```

- [ ] **Step 2: Register the test.** In `tests/CMakeLists.txt`, add `test_dolphin_netplay_e2e.cpp` after `test_dolphin_savestate_e2e.cpp`.

- [ ] **Step 3: Build tests; run the gated test; verify it FAILS red.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build && cmake --build . --config Debug --target retropark_tests && cd tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin netplay: two processes*"
```
Expected: FAILS — `GetProcAddress` returns null for `rp_dolphin_netplay_host` (not exported yet) → the `REQUIRE(a.host)` fails. (WARN-skip means the DLL is missing — not the expected red.)

- [ ] **Step 4: Add the NetPlayUI stub + C API to the vehicle.** In `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp`, add includes:

```cpp
#include "Core/NetPlayServer.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "UICommon/GameFile.h"
```

Add a headless `NetPlayUI` implementation — **mirror `DolphinQt/NetPlay/NetPlayDialog.cpp`'s method behaviors, but headless**: no-op/log the display/chat/digest/GBA/Wii-sync methods; implement the connection/state callbacks to set atomic flags. Skeleton (fill EVERY pure-virtual of `NetPlayUI` — read `Core/NetPlayClient.h:45` for the full list; the ones below are load-bearing, the rest are `{}`/return-default):

```cpp
namespace {
std::atomic<int> g_np_connected{0}, g_np_started{0}, g_np_desynced{0};
std::atomic<uint32_t> g_np_players{0};

class RpNetPlayUI final : public NetPlay::NetPlayUI {
public:
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override;   // Task 2 fills this
  void StopGame() override {}
  bool IsHosting() const override { return g_netplay_server != nullptr; }
  void Update() override {}
  void AppendChat(const std::string&) override {}
  void OnMsgChangeGame(const NetPlay::SyncIdentifier& id, const std::string&) override { m_current_id = id; }
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}
  void OnMsgStartGame() override;                                               // Task 2 fills this
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string&) override { g_np_players.fetch_add(1); }
  void OnPlayerDisconnect(const std::string&) override { if (g_np_players) g_np_players.fetch_sub(1); }
  void OnPadBufferChanged(u32) override {}
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32, const std::string&) override { g_np_desynced.store(1); }
  void OnConnectionLost() override { g_np_connected.store(0); }
  void OnConnectionError(const std::string&) override {}
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override {}
  void OnGolferChanged(bool, const std::string&) override {}
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
      FindGameFile(const NetPlay::SyncIdentifier&, NetPlay::SyncIdentifierComparison*) override;  // Task 2
  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override { return {}; }
  void ShowGameDigestDialog(const std::string&) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string&) override {}
  void AbortGameDigest() override {}
  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}
  void ShowChunkedProgressDialog(const std::string&, u64, const std::vector<int>&) override {}
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64>, std::string) override {}
  NetPlay::SyncIdentifier m_current_id{};
};
RpNetPlayUI g_netplay_ui;
std::unique_ptr<NetPlay::NetPlayServer> g_netplay_server;
std::unique_ptr<NetPlay::NetPlayClient> g_netplay_client;
}  // namespace
```

**Verify the exact `NetPlayUI` signatures against `Core/NetPlayClient.h` — some argument types (e.g. `SyncIdentifierComparison*`, `std::vector<int>&`) must match precisely or it won't compile as an override.**

Then the C API (extern "C", near the Slice-J `rp_dolphin_*` exports):

```cpp
extern "C" {
__declspec(dllexport) int rp_dolphin_netplay_host(uint16_t port) {
  g_netplay_server = std::make_unique<NetPlay::NetPlayServer>(port, /*forward_port=*/false, &g_netplay_ui,
                                                              NetPlay::NetTraversalConfig{});
  if (!g_netplay_server || !g_netplay_server->is_connected) { g_netplay_server.reset(); return 1; }
  g_np_players.store(1);   // host counts itself
  return 0;
}
__declspec(dllexport) int rp_dolphin_netplay_join(const char* ip, uint16_t port) {
  g_netplay_client = std::make_unique<NetPlay::NetPlayClient>(std::string(ip ? ip : ""), port, &g_netplay_ui,
                                                              "player2", NetPlay::NetTraversalConfig{});
  if (!g_netplay_client) return 1;
  g_np_connected.store(g_netplay_client->IsConnected() ? 1 : 0);
  return g_netplay_client->IsConnected() ? 0 : 1;
}
__declspec(dllexport) void rp_dolphin_netplay_status(int* c, int* s, int* d, uint32_t* p) {
  if (g_netplay_client) g_np_connected.store(g_netplay_client->IsConnected() ? 1 : 0);
  if (c) *c = g_np_connected.load(); if (s) *s = g_np_started.load();
  if (d) *d = g_np_desynced.load(); if (p) *p = g_np_players.load();
}
__declspec(dllexport) void rp_dolphin_netplay_stop() {
  g_netplay_client.reset(); g_netplay_server.reset();
  g_np_connected.store(0); g_np_started.store(0); g_np_desynced.store(0); g_np_players.store(0);
}
}
```

**Note:** verify `NetPlayServer` exposes a connection-success signal — `is_connected` is the member DolphinQt checks (`Core/NetPlayServer.h`); if the member name differs, use whatever DolphinQt reads after `new NetPlayServer(...)`. Give `BootGame`/`OnMsgStartGame`/`FindGameFile` minimal bodies for now (e.g. `BootGame` → `{}`, `OnMsgStartGame` → `{}`, `FindGameFile` → `return nullptr;`) so it compiles; Task 2 implements them.

- [ ] **Step 5: Relink the DLL (PowerShell).**

```bash
powershell -NoProfile -Command '& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\Core\DolphinNoGUI\RetroParkDolphin.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:SolutionDir="C:\Users\cubma\source\repos\RetroPark\external\dolphin\Source\\" -p:BuildProjectReferences=false -m -v:minimal -nologo'
```
Expected: relinks. If `NetPlayServer`/`NetPlayClient`/`NetPlayUI` symbols are unresolved at link, they live in DolphinLib (already linked by the DLL) — a link error instead means a missing lib; confirm `NetPlayServer.cpp` is part of DolphinLib (it is). Fix any override-signature compile errors against `Core/NetPlayClient.h`. Confirm fresh DLL timestamp + `rp_get_core_abi` still exported (dumpbin).

- [ ] **Step 6: Verify the netplay exports.**

```bash
powershell -NoProfile -Command '$d=Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe | Select -First 1 -Expand FullName; & $d /exports "C:\Users\cubma\source\repos\RetroPark\external\dolphin\Binary\x64\dolphin_present.dll" | Select-String "rp_dolphin_netplay"'
```
Expected: `rp_dolphin_netplay_host/join/status/stop` listed.

- [ ] **Step 7: Run the gated CONNECT test; verify it PASSES.**

```bash
cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin netplay: two processes*"
```
Expected: `1 passed` — host sees `players >= 2`, child exits 0 (connected). If the child hangs (never connects), check the port isn't firewalled on loopback and that `NetPlayServer` bound successfully (its `is_connected`). If `players` never reaches 2, the `OnPlayerConnect` callback name/signature may differ — verify against `Core/NetPlayClient.h`.

- [ ] **Step 8: Refresh the patch + full suite + commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git -C external/dolphin add -N Source/Core/DolphinNoGUI/rp_dolphin.cpp 2>/dev/null; git -C external/dolphin diff > docs/patches/dolphin-external-present.patch 2>/dev/null; grep -c "rp_dolphin_netplay\|NetPlayUI" docs/patches/dolphin-external-present.patch; grep -c "warning:" docs/patches/dolphin-external-present.patch
cmake --build build --config Debug && ./build/tests/Debug/retropark_tests.exe   # expect 104 passed | 0 failed (new case skips w/o env)
git add tests/test_dolphin_netplay_e2e.cpp tests/CMakeLists.txt docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): Slice O task 1 — headless NetPlayUI stub + netplay host/join C API (two processes connect over loopback)"
```
Expected: first grep > 0, second `0`; suite `104 passed | 0 failed`.

---

### Task 2: Synchronized boot + desync-free play (the netplay proof)

Add `rp_dolphin_netplay_start` (host arms the game) and implement the boot bridge (`OnMsgStartGame`/`FindGameFile`/`BootGame`) so both peers boot the ROM via Dolphin netplay; extend the test to boot both and run N frames desync-free.

**Files:**
- Modify: `external/dolphin/Source/Core/DolphinNoGUI/rp_dolphin.cpp` (netplay_start + boot bridge + boot-flow fork)
- Modify: `tests/test_dolphin_netplay_e2e.cpp` (extend to boot + N-frame desync check)
- Modify: `docs/patches/dolphin-external-present.patch` (regenerate)

**Interfaces:**
- Consumes: `NetPlayServer::ChangeGame(const SyncIdentifier&, const std::string&)`, `NetPlayServer::RequestStartGame()`, `NetPlayClient::StartGame(const std::string& path)`, `UICommon::GameFile(path).GetSyncIdentifier()`/`.GetFilePath()`, `BootParameters::GenerateFromFile(path, BootSessionData)`, `BootManager::BootCore`. Reference: `DolphinQt/NetPlay/NetPlayDialog.cpp` `OnMsgStartGame`/`BootGame`/`FindGameFile`.
- Produces: `int rp_dolphin_netplay_start()` (host); the boot bridge; the netplay boot-flow fork in `HostThread`.

- [ ] **Step 1: Extend the test to require boot + no desync.** In `tests/test_dolphin_netplay_e2e.cpp`, add a SECOND gated test case (leave the connect case as-is) — or extend the existing one — that after connecting: host calls `rp_dolphin_netplay_start()`, both boot the ROM, and both poll `started==1` and `desynced==0` over N status polls (~a few seconds). Add `start_fn` (`typedef int (*start_fn)()`) to `NetApi`, and a ROM-exists gate + a `rp_runtime`/present or the vehicle boot to actually run frames. Concretely, add:

```cpp
TEST_CASE("dolphin netplay: two processes boot in sync and stay desync-free (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(kDll)) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists("C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz")) { WARN("ROM absent; skipping"); return; }
    // Parent hosts + arms; child joins; both boot via netplay; assert started && !desynced over N polls.
    // (Mirror the Task-1 role-split; add a.start() on the host after players>=2, and both sides boot the
    //  ROM through the netplay path. The exact per-side boot call comes from Task 2's C API — the netplay
    //  BootGame drives it, triggered by netplay_start. Poll status for started==1 and desynced==0.)
}
```

(The implementer completes this test body once the Task-2 C API/boot exists; the assertions are: both sides reach `started==1`, and `desynced` stays 0 across the N-poll window. Child signals via exit code; parent asserts its own status + the child's.)

- [ ] **Step 2: Run it; verify it FAILS red.** `serialize`… no — run the new case gated; it fails because `rp_dolphin_netplay_start` doesn't exist / the game never boots (`started` stays 0).

Run: `cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin netplay: two processes boot*"`
Expected: FAIL (no `rp_dolphin_netplay_start` export / `started` never 1).

- [ ] **Step 3: Implement `rp_dolphin_netplay_start` (host).** Mirror `NetPlayDialog.cpp:390`+`489`: build a `UICommon::GameFile` for the ROM the vehicle was armed with, then `g_netplay_server->ChangeGame(game.GetSyncIdentifier(), "netplay")` and `g_netplay_server->RequestStartGame()`:

```cpp
extern "C" __declspec(dllexport) int rp_dolphin_netplay_start() {
  if (!g_netplay_server) return 1;
  UICommon::GameFile game(g_netplay_rom);   // g_netplay_rom set when arming/booting (see boot fork)
  if (!g_netplay_server->ChangeGame(game.GetSyncIdentifier(), "netplay")) return 2;
  return g_netplay_server->RequestStartGame() ? 0 : 3;
}
```

Store the ROM path (`std::string g_netplay_rom`) — set it in the netplay arming (Task 1's host/join can take the ROM, OR reuse the existing content path the vehicle already knows). Keep it consistent with how the test passes the ROM.

- [ ] **Step 4: Implement the boot bridge (`OnMsgStartGame`, `FindGameFile`, `BootGame`).** Mirror `NetPlayDialog.cpp` `OnMsgStartGame` (871) and `BootGame` (768):

```cpp
std::shared_ptr<const UICommon::GameFile>
RpNetPlayUI::FindGameFile(const NetPlay::SyncIdentifier& id, NetPlay::SyncIdentifierComparison* cmp) {
  auto game = std::make_shared<UICommon::GameFile>(g_netplay_rom);
  if (cmp) *cmp = game->CompareSyncIdentifier(id);   // verify the method name against GameFile.h
  return game;
}
void RpNetPlayUI::OnMsgStartGame() {
  g_np_started.store(1);
  if (g_netplay_client) {
    auto game = FindGameFile(m_current_id, nullptr);
    if (game) g_netplay_client->StartGame(game->GetFilePath());   // client builds BootSessionData -> BootGame
  }
}
void RpNetPlayUI::BootGame(const std::string& filename,
                           std::unique_ptr<BootSessionData> boot_session_data) {
  // Boot with the netplay-provided BootSessionData + our hidden window / producer / audio / input, exactly
  // as HostThread does for a normal boot but with THIS BootSessionData (which carries the netplay settings).
  // Signal HostThread (which owns the window/wsi/producer) to run BootManager::BootCore with these args.
  RpBootRequest req{filename, std::move(boot_session_data)};
  g_boot_request.set(std::move(req));   // HostThread waits on this in netplay mode and boots
}
```

**The boot-flow fork:** in `HostThread`, when netplay is armed, do the window/WSI/producer/audio/input setup as today, but instead of directly `BootManager::BootCore(...)`, wait for `g_boot_request` (set by `BootGame`) and then `BootManager::BootCore(system, BootParameters::GenerateFromFile(req.filename, std::move(*req.boot_session_data)), wsi)`. The rest of `HostThread` (run loop, audio puller, input override, teardown) is unchanged. The non-netplay path is exactly as before. **Mirror how `DolphinNoGUI`/`NetPlayDialog` bridge `BootGame` to `BootManager::BootCore`; the `m_start_game_callback` in Qt is the seam — here the seam is `HostThread` picking up `g_boot_request`.**

- [ ] **Step 5: Arm netplay in the boot flow.** The vehicle must know it's in netplay mode (so `HostThread` waits for `g_boot_request` instead of booting directly) and know `g_netplay_rom`. Set an atomic `g_netplay_armed` in `rp_dolphin_netplay_host`/`join` and store the ROM (from the arming call or the content path). Ensure the host also calls `rp_dolphin_netplay_start()` after the client connects (the TEST orchestrates this; the harness does too in Task 3).

- [ ] **Step 6: Relink the DLL, refresh patch, run the gated boot test.** (Same PowerShell relink as Task 1 Step 5; patch regen stdout-only.)

Run: `cd C:/Users/cubma/source/repos/RetroPark/build/tests/Debug && RP_RUN_DOLPHIN=1 ./retropark_tests.exe --test-case="dolphin netplay*"`
Expected: BOTH netplay cases pass — connect (Task 1) + boot-in-sync (`started==1` on both, `desynced==0` over the N-poll window). If `desynced` fires immediately, the two peers booted from different states/settings — check `SetupNetSettings` ran (the server does it in `RequestStartGame`/`StartGame`; verify against `NetPlayServer.cpp`) and both use identical Dolphin config. If boot never starts, verify `OnMsgStartGame → StartGame → BootGame → g_boot_request → HostThread boot` chain with stderr logging at each step.

- [ ] **Step 7: Full suite + commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && cmake --build build --config Debug && ./build/tests/Debug/retropark_tests.exe   # 105 passed | 0 failed (both netplay cases skip w/o env)
git add tests/test_dolphin_netplay_e2e.cpp docs/patches/dolphin-external-present.patch && git commit -m "feat(dolphin): Slice O task 2 — netplay synchronized boot + desync-free play (drive Dolphin's netplay start/boot)"
```

---

### Task 3: Harness netplay flags for the Dolphin core

Let a human play 2-player: `--netplay-host <port>` / `--netplay-join <ip:port>` on the `--core <dolphin>` path call the vehicle netplay C API.

**Files:**
- Modify: `harness/windowed/main.cpp` (parse the flags; `GetProcAddress` the vehicle netplay API on the loaded core DLL; arm host/join around content load; host calls `netplay_start` once connected)

**Interfaces:**
- Consumes: the vehicle exports `rp_dolphin_netplay_host/join/start/status/stop` (via `GetModuleHandle`/`LoadLibraryEx` on the core DLL path + `GetProcAddress`); existing `--core`/`--content` handling.
- Produces: nothing for later tasks (harness-only).

- [ ] **Step 1: Parse the flags + resolve the API.** In `main.cpp`, parse `--netplay-host <port>` → `np_host_port`, `--netplay-join <ip:port>` → `np_join_addr` (reuse the Slice-G parsing style already present for the driven netplay flags if compatible; otherwise add). After `rp_runtime_load_core(custom_core_dir)`, `LoadLibraryExA` the same `custom_core_dir + "/dolphin_present.dll"` (already loaded by the Runtime; this just gets a handle) and `GetProcAddress` the five netplay fns.

- [ ] **Step 2: Arm + drive.** Before/around `rp_runtime_load_content`: if `--netplay-host`, call `netplay_host(port)`; if `--netplay-join`, `netplay_join(ip, port)`. For the host, after `load_content`, poll `netplay_status` until `players>=2` then call `netplay_start()`. (The Runtime's present loop then drives frames as usual — Dolphin netplay runs underneath.) Guard all of this behind `!custom_core_dir.empty()` so the refcore/driven paths are untouched.

```cpp
    // (Dolphin netplay, --core path only) arm the session; the Runtime present loop drives frames.
    if (!custom_core_dir.empty() && (np_host_port || !np_join_addr.empty())) {
        HMODULE core = LoadLibraryExA((custom_core_dir + "\\dolphin_present.dll").c_str(), nullptr,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
        auto host = (int(*)(uint16_t))GetProcAddress(core, "rp_dolphin_netplay_host");
        auto join = (int(*)(const char*, uint16_t))GetProcAddress(core, "rp_dolphin_netplay_join");
        auto start = (int(*)())GetProcAddress(core, "rp_dolphin_netplay_start");
        auto status = (void(*)(int*,int*,int*,uint32_t*))GetProcAddress(core, "rp_dolphin_netplay_status");
        if (np_host_port && host) host(np_host_port);
        else if (!np_join_addr.empty() && join) { /* split ip:port */ join(ip.c_str(), port); }
        // host: after content load + peer connect, call start() (poll status for players>=2) — do this
        // after rp_runtime_load_content below, on a short timer in the message loop.
    }
```

(Wire the `start()` call into the main loop for the host once `players>=2`; keep it minimal — this path is the human-play convenience, not gated by a test.)

- [ ] **Step 3: Build + full suite.**

Run: `cmake --build C:/Users/cubma/source/repos/RetroPark/build --config Debug && C:/Users/cubma/source/repos/RetroPark/build/tests/Debug/retropark_tests.exe`
Expected: harness links; suite `105 passed | 0 failed`.

- [ ] **Step 4: Commit.**

```bash
cd C:/Users/cubma/source/repos/RetroPark && git add harness/windowed/main.cpp && git commit -m "feat(harness): --netplay-host/--netplay-join for the Dolphin core (drive vehicle netplay C API)"
```

---

## Post-plan: verify + merge + memory

After Task 2 is green and reviewed: full suite green + the gated 2-process netplay proof (connect + sync-boot + desync-free over N polls), then merge to `main` + push `origin main` (no finish-branch menu, no AI attribution), then update memory (`retropark-project.md` + `MEMORY.md`) marking Slice O done — noting it drives Dolphin's built-in netplay via a headless NetPlayUI stub + vehicle C API, localhost-2-process-proven, and that **real cross-machine LAN is user-verified-pending** (like Slice G/H). Flag the deferred list (4P, Wii, traversal/NAT, spectators, Runtime netplay API).
