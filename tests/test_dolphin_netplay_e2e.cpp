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
