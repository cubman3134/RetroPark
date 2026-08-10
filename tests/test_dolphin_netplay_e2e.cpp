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

// Slice O task 1: RetroPark drives Dolphin's OWN built-in netplay (proven determinism/lockstep). This
// gated test stands up a headless netplay session and proves two processes connect over loopback — no
// game boot yet. The parent hosts (rp_dolphin_netplay_host), then re-execs the test binary with
// RP_NETPLAY_ROLE=join; the child joins (rp_dolphin_netplay_join) and exits 0 once connected. Host
// asserts it sees itself + the joiner (players >= 2); child asserts it connected.

using namespace rp;
namespace {
const char* kDll = "C:/Users/cubma/source/repos/RetroPark/external/dolphin/Binary/x64/dolphin_present.dll";
bool file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

typedef int (*host_fn)(uint16_t);
typedef int (*join_fn)(const char*, uint16_t);
typedef void (*status_fn)(int*, int*, int*, uint32_t*, uint32_t*);   // + frame-progress counter (Slice O fix)
typedef void (*stop_fn)();
typedef int (*arm_fn)(const char*);   // Task 2: set the local ROM + start the netplay-armed host thread
typedef int (*start_fn)();            // Task 2: host arms the game (ChangeGame + RequestStartGame)

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
const uint16_t kPort2 = 54701;   // boot test uses a distinct port from the connect test
const char* kRom =
    "C:/RetroBat/roms/gamecube/Billy Hatcher and the Giant Egg (USA)/Billy Hatcher and the Giant Egg (USA).rvz";
} // namespace

TEST_CASE("dolphin netplay: two processes connect over loopback (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping"); return; }
    if (!file_exists(kDll)) { WARN("dolphin_present.dll not built; skipping"); return; }

    if (std::getenv("RP_NETPLAY_ROLE")) {
        // Child = joiner. Connect to the host, wait to be connected, exit(0) on success / exit(2) on fail.
        NetApi a = load_net();
        int rc = a.join("127.0.0.1", kPort);
        int connected = 0, started = 0, desynced = 0; uint32_t players = 0, frame = 0;
        for (int i = 0; i < 100 && !connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            a.status(&connected, &started, &desynced, &players, &frame);
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

    int connected = 0, started = 0, desynced = 0; uint32_t players = 0, frame = 0;
    for (int i = 0; i < 100 && players < 2; ++i) {   // host sees itself + the joiner
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        a.status(&connected, &started, &desynced, &players, &frame);
    }
    CHECK(players >= 2);

    WaitForSingleObject(pi.hProcess, 15000);
    DWORD child_rc = 1; GetExitCodeProcess(pi.hProcess, &child_rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    a.stop();
    CHECK(child_rc == 0);   // joiner connected
}

// Slice O task 2: the netplay PROOF. Both peers arm the same ROM, connect, and the host arms the game
// (rp_dolphin_netplay_start). Dolphin's own netplay start/boot flow (OnMsgStartGame -> client StartGame
// -> BootGame -> HostThread BootCore) boots the SAME game on both processes with the SAME net-synced
// settings, then runs it in lockstep. We assert BOTH peers actually reach a running game (started==1)
// and that the server's per-frame timebase comparison never fires a desync (desynced==0) over a
// multi-second running window. started==1 is set only AFTER BootCore succeeds, so it is an honest
// "the game booted and is running" signal — a silent boot failure can't produce a false desync-free pass
// (a non-running peer sends no timebase, so no comparison happens; the child's exit code guards its side).
TEST_CASE("dolphin netplay: two processes boot in sync and stay desync-free (gated)") {
    if (!std::getenv("RP_RUN_DOLPHIN")) { WARN("RP_RUN_DOLPHIN not set; skipping"); return; }
    if (!VulkanBackend::probe_vulkan_shared()) { WARN("no capable Vulkan device; skipping"); return; }
    if (!file_exists(kDll)) { WARN("dolphin_present.dll not built; skipping"); return; }
    if (!file_exists(kRom)) { WARN("ROM absent; skipping"); return; }

    // A poll helper: returns once `started` is seen (or the deadline lapses); flips `desynced` if the
    // server ever reports a desync during the wait. ~90s budget covers the two ~40s netplay boots.
    auto wait_started = [](NetApi& a, bool& started, bool& desynced, int max_polls) {
        int c = 0, s = 0, d = 0; uint32_t p = 0, f = 0;
        for (int i = 0; i < max_polls && !started; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            a.status(&c, &s, &d, &p, &f);
            if (s) started = true;
            if (d) desynced = true;
        }
    };
    // Watch a running game for a fixed window, asserting the desync flag stays clear the whole time.
    auto watch_desync = [](NetApi& a, bool& desynced, int polls) {
        int c = 0, s = 0, d = 0; uint32_t p = 0, f = 0;
        for (int i = 0; i < polls; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            a.status(&c, &s, &d, &p, &f);
            if (d) desynced = true;
        }
    };
    // Sample the presented-frame counter (frame-progress proof). If netplay input exchange broke, both
    // CPU threads block in GetNetPads at frame ~0 -> no frames present -> this never advances -> the
    // desync comparison never runs and desynced==0 would pass VACUOUSLY on two frozen games. Asserting
    // strict advance closes that hole.
    auto sample_frame = [](NetApi& a) -> uint32_t {
        int c = 0, s = 0, d = 0; uint32_t p = 0, f = 0;
        a.status(&c, &s, &d, &p, &f);
        return f;
    };

    if (std::getenv("RP_NETPLAY_ROLE")) {
        // Child = joiner. Arm the ROM, join, wait to boot, then watch for desync; signal via exit code.
        NetApi a = load_net();
        arm_fn arm = (arm_fn)GetProcAddress(a.dll, "rp_dolphin_netplay_arm");
        if (!arm) std::exit(3);                       // Task-2 export missing (red)
        if (arm(kRom) != 0) std::exit(4);
        int rc = a.join("127.0.0.1", kPort2);
        int c = 0, s = 0, d = 0; uint32_t p = 0, f = 0;
        for (int i = 0; i < 100 && !c; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            a.status(&c, &s, &d, &p, &f);
        }
        bool started = false, desynced = false;
        wait_started(a, started, desynced, 900);      // up to ~90s for the netplay boot
        bool frames_advanced = false;
        if (started) {
            uint32_t frame_start = sample_frame(a);
            watch_desync(a, desynced, 100);           // ~10s running, desync-free
            uint32_t frame_end = sample_frame(a);
            frames_advanced = (frame_end > frame_start + 10);   // frames actually ADVANCED, not frozen
        }
        a.stop();
        std::exit((rc == 0 && started && !desynced && frames_advanced) ? 0 : 2);
    }

    // Parent = host.
    NetApi a = load_net();
    arm_fn arm = (arm_fn)GetProcAddress(a.dll, "rp_dolphin_netplay_arm");
    start_fn start = (start_fn)GetProcAddress(a.dll, "rp_dolphin_netplay_start");
    REQUIRE(arm); REQUIRE(start);                     // Task-2 exports (red until implemented)
    REQUIRE(arm(kRom) == 0);
    REQUIRE(a.host(kPort2) == 0);

    // Spawn ourselves as the joiner (same boot test-case filter + role env).
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string cmd = std::string("\"") + exe + "\" --test-case=\"dolphin netplay: two processes boot*\"";
    STARTUPINFOA si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    SetEnvironmentVariableA("RP_NETPLAY_ROLE", "join");
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableA("RP_NETPLAY_ROLE", nullptr);
    REQUIRE(ok);

    // Wait for the joiner to connect (host sees itself + the joiner), then arm the game.
    int c = 0, s = 0, d = 0; uint32_t p = 0, f = 0;
    for (int i = 0; i < 100 && p < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        a.status(&c, &s, &d, &p, &f);
    }
    REQUIRE(p >= 2);
    REQUIRE(start() == 0);                            // host: ChangeGame + RequestStartGame

    bool started = false, desynced = false;
    wait_started(a, started, desynced, 900);          // up to ~90s for our own netplay boot
    CHECK(started);                                   // the game actually booted + is running
    uint32_t frame_start = 0, frame_end = 0;
    if (started) {
        frame_start = sample_frame(a);
        watch_desync(a, desynced, 100);               // ~10s running, desync-free
        frame_end = sample_frame(a);
    }
    CHECK(!desynced);                                 // server saw no timebase divergence
    CHECK(frame_end > frame_start + 10);              // frames actually ADVANCED (not a frozen game)

    WaitForSingleObject(pi.hProcess, 180000);
    DWORD child_rc = 1; GetExitCodeProcess(pi.hProcess, &child_rc);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    a.stop();
    CHECK(child_rc == 0);   // joiner also booted + stayed desync-free
}
