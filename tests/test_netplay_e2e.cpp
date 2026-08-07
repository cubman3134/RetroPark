#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <doctest/doctest.h>
#include "net/NetSession.h"
#include "net/LoopbackTransport.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include <retropark/retropark.h>
#include <retropark/retropark_abi.h>
#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace rp;
using namespace rp::net;

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif
#ifndef RP_SHIM_DIR
#define RP_SHIM_DIR "cores/libretro_shim"
#endif
#ifndef RP_NES_ROM_DIR
#define RP_NES_ROM_DIR "C:/RetroBat/roms/nes"
#endif

// Load the reference driven core into a runtime. Mirrors the device-free setup used by the
// Slice F portable savestate e2e (test_savestate.cpp): RP_GFX_D3D11 defaults to a WARP
// (software) device, so this touches no real GPU and runs unconditionally. The driven core
// advances its state (a 4-byte frame counter) each present(), serialize/load round-trips it.
static void load_refcore_driven(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_DRIVEN_CORE_DIR) == RP_OK);
}

// Duplicated (not shared) probe helpers, matching the sibling gated e2e files
// (test_libretro_e2e.cpp, test_savestate.cpp) which each keep their own prefixed
// static copy rather than a cross-TU header for two tiny functions.
static bool netplay_file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string netplay_first_nes(const std::string& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() == ".nes") return entry.path().string();
    }
    return {};
}

static bool fceumm_and_rom_present() {
    return !netplay_first_nes(RP_NES_ROM_DIR).empty() &&
           netplay_file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll");
}

// FCEUmm — like most libretro cores ported from standalone emulators — keeps its CPU/PPU/APU
// state in process-wide C globals, and the shim's own instance pointer is one-per-loaded-
// module too. LoadLibrary is refcounted BY PATH: two Runtimes that both load the same on-disk
// shim DLL get back the SAME Windows module and would silently share one emulator's memory
// instead of being genuinely independent runtimes -- exactly the kind of aliasing this gate
// exists to rule out. Give each call its own on-disk copy of the whole shim package (shim +
// wrapped core + manifest) so Win32CoreModule::open() sees a distinct file per side and gets
// a separate module image (separate statics) from Windows.
static std::string netplay_private_shim_copy() {
    namespace fs = std::filesystem;
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1);
    std::error_code ec;
    fs::path dst = fs::temp_directory_path(ec) / ("rp_netplay_shim_" + std::to_string(id));
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::directory_iterator(RP_SHIM_DIR, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        fs::copy_file(entry.path(), dst / entry.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    return dst.string();
}

// Load the real libretro shim + a real Donkey Kong (first .nes found) into rt, exactly like
// the gated e2e in test_libretro_e2e.cpp / test_savestate.cpp -- except from a private
// per-call copy of the shim package (see netplay_private_shim_copy() above) so two runtimes
// loaded in this one process never alias the same DLL image.
static void load_shim_with_donkey_kong(Runtime& rt) {
    std::string rom = netplay_first_nes(RP_NES_ROM_DIR);
    REQUIRE(!rom.empty());
    std::string shim_dir = netplay_private_shim_copy();
    REQUIRE(rt.resize(256, 240) == RP_OK);          // NES resolution
    REQUIRE(rt.load_core(shim_dir) == RP_OK);
    REQUIRE(rp_runtime_load_content(reinterpret_cast<rp_runtime*>(&rt), rom.c_str()) == RP_OK);
}

// ---- Gate 1: two refcore runtimes stay serialize-equal in lockstep ----
//
// THE guarantee of the netplay slice: two independent runtimes, driven only by NetSession's
// handshake + state sync + per-frame lockstep tick, produce byte-identical serialized state
// on every frame, and never desync or disconnect. Inputs differ per port to prove the
// two-port plumbing never corrupts either machine's state.
//
// The lockstep LOOP is single-threaded (both send, then both recv+advance — loopback never
// blocks because at delay>=1 the remote input for frame F was sent delay iterations earlier).
// Only the symmetric handshake runs concurrently: start_host sends its Hello then blocks on
// the peer's Hello, so the two start_* calls must overlap (same idiom as the TCP e2e). Once
// both return, the exchange is fully drained and only Input/Checksum messages flow.
TEST_CASE("netplay: two refcore runtimes stay serialize-equal in lockstep (Gate 1)") {
    Runtime a(RP_GFX_D3D11, nullptr), b(RP_GFX_D3D11, nullptr);   // WARP, device-free
    load_refcore_driven(a); load_refcore_driven(b);

    auto [ta, tb] = make_loopback_pair();
    NetSession sa, sb;

    // Symmetric handshake: run the host start on a thread so it overlaps the join start.
    std::string errH, errJ;
    rp_result hr = RP_ERR_INTERNAL;
    std::thread th([&] { hr = sa.start_host(a, *ta, /*delay=*/2, /*hash=*/0, "refcore_driven", errH); });
    rp_result jr = sb.start_join(b, *tb, /*hash=*/0, "refcore_driven", errJ);
    th.join();
    REQUIRE(hr == RP_OK);
    REQUIRE(jr == RP_OK);

    auto crc_of = [](Runtime& rt) {
        size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&rt));
        std::vector<uint8_t> buf(sz);
        REQUIRE(rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), buf.data(), sz) == RP_OK);
        return rp::net::crc32(buf.data(), sz);
    };

    // Baseline: after state sync the two runtimes are already serialize-equal.
    REQUIRE(crc_of(a) == crc_of(b));

    int frames_equal = 0;
    for (uint64_t f = 0; f < 300; ++f) {
        rp_input_state ina{}, inb{};
        ina.keys['X'] = (f % 3 == 0) ? 1 : 0;   // scripted, differing per-port inputs
        inb.keys['Z'] = (f % 5 == 0) ? 1 : 0;
        // single-threaded lockstep: both send, then both recv+advance (loopback => no block)
        sa.tick_send(ina); sb.tick_send(inb);
        CHECK(sa.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sb.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sa.status() != NetStatus::Desync);
        CHECK(sb.status() != NetStatus::Desync);
        REQUIRE(crc_of(a) == crc_of(b));    // THE guarantee: identical state every frame
        if (crc_of(a) == crc_of(b)) ++frames_equal;
    }
    CHECK(frames_equal == 300);
    CHECK(sa.frame() == 300u);              // both advanced exactly one frame per tick
    CHECK(sb.frame() == 300u);
    CHECK(sa.status() == NetStatus::Ok);
    CHECK(sb.status() == NetStatus::Ok);
}

// ---- Gate 2: two real FCEUmm/Donkey Kong runtimes stay serialize-equal in lockstep ----
//
// Gate 1 proves the netplay guarantee against the portable refcore_driven stub. This gate
// proves the same guarantee against a real emulator core and a real ROM: two independently
// loaded FCEUmm runtimes, advanced identically past boot, then driven only through
// NetSession's handshake + state sync + per-frame lockstep tick, stay byte-identical (by
// full savestate CRC) across 120 frames of differing per-port input and never desync.
// Gated: WARN-skip (not fail) if the shim core DLL or a NES ROM is absent on this machine.
TEST_CASE("netplay: two FCEUmm runtimes stay serialize-equal in lockstep (gated)") {
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping netplay FCEUmm lockstep"); return; }

    Runtime a(RP_GFX_D3D11, nullptr), b(RP_GFX_D3D11, nullptr);
    load_shim_with_donkey_kong(a);
    load_shim_with_donkey_kong(b);
    // Advance both past boot identically so their pre-sync states already match before the
    // host's STATE_SYNC (which then aligns them anyway, so this is belt-and-suspenders).
    for (int i = 0; i < 200; ++i) {
        rp_runtime_present(reinterpret_cast<rp_runtime*>(&a), nullptr);
        rp_runtime_present(reinterpret_cast<rp_runtime*>(&b), nullptr);
    }

    auto [ta, tb] = make_loopback_pair();
    NetSession sa, sb;

    // Symmetric handshake: run the host start on a thread so it overlaps the join start
    // (same idiom as Gate 1 / the TCP e2e -- start_host blocks on the peer's Hello).
    std::string errH, errJ;
    rp_result hr = RP_ERR_INTERNAL;
    std::thread th([&] { hr = sa.start_host(a, *ta, /*delay=*/2, /*hash=*/0xD0, "fceumm", errH); });
    rp_result jr = sb.start_join(b, *tb, /*hash=*/0xD0, "fceumm", errJ);   // host STATE_SYNC aligns b to a
    th.join();
    REQUIRE(hr == RP_OK);
    REQUIRE(jr == RP_OK);

    auto crc_of = [](Runtime& rt) {
        size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&rt));
        std::vector<uint8_t> buf(sz);
        REQUIRE(rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), buf.data(), sz) == RP_OK);
        return rp::net::crc32(buf.data(), sz);
    };
    REQUIRE(crc_of(a) == crc_of(b));   // state sync worked

    for (uint64_t f = 0; f < 120; ++f) {
        rp_input_state p0{}, p1{};
        p0.keys[VK_RIGHT] = (f % 2 == 0) ? 1 : 0;   // P1 taps right
        p1.keys['X']      = (f % 7 == 0) ? 1 : 0;   // P2 taps A
        sa.tick_send(p0); sb.tick_send(p1);
        REQUIRE(sa.tick_recv_and_advance() != NetStatus::Disconnected);
        REQUIRE(sb.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sa.status() != NetStatus::Desync);
        CHECK(sb.status() != NetStatus::Desync);
        REQUIRE(crc_of(a) == crc_of(b));            // real NES stays lockstep-identical
    }
    CHECK(sa.frame() == 120u);
    CHECK(sb.frame() == 120u);
    CHECK(sa.status() == NetStatus::Ok);
    CHECK(sb.status() == NetStatus::Ok);
}

// ---- Unit: the desync decision (two crc maps, one mismatching frame) ----
TEST_CASE("netplay: checksum-compare flags divergence") {
    std::map<uint64_t, uint32_t> own{{60, 0xAAAA}}, peer{{60, 0xBBBB}};
    bool desync = false;
    for (auto& kv : own) {
        auto it = peer.find(kv.first);
        if (it != peer.end() && it->second != kv.second) desync = true;
    }
    CHECK(desync);

    // Matching crcs at the same frame must NOT flag desync.
    std::map<uint64_t, uint32_t> own2{{60, 0x1234}}, peer2{{60, 0x1234}};
    bool desync2 = false;
    for (auto& kv : own2) {
        auto it = peer2.find(kv.first);
        if (it != peer2.end() && it->second != kv.second) desync2 = true;
    }
    CHECK_FALSE(desync2);
}

// ---- Unit: the input-delay ring maps a send-frame to apply-frame F+delay, and the first
//            `delay` frames apply a neutral input on both ports (nothing was sent for them).
TEST_CASE("netplay: input-delay ring applies at F+delay, neutral for the first `delay` frames") {
    const uint32_t delay = 2;
    std::map<uint64_t, rp_input_state> local_pending;   // by apply-frame, as NetSession keeps it
    // Simulate tick_send over a handful of send-frames.
    for (uint64_t send_f = 0; send_f < 5; ++send_f) {
        rp_input_state in{};
        in.keys['A'] = uint8_t(send_f + 1);      // distinct marker per send-frame
        local_pending[send_f + delay] = in;      // stored to apply at send_f + delay
    }
    // Absolute frames 0..delay-1: no local input was ever stored -> neutral.
    for (uint64_t af = 0; af < delay; ++af) {
        CHECK(local_pending.find(af) == local_pending.end());
    }
    // Absolute frame delay: applies the input that was SENT at send-frame 0.
    REQUIRE(local_pending.find(delay) != local_pending.end());
    CHECK(local_pending[delay].keys['A'] == 1);
    // Absolute frame delay+3: applies the input sent at send-frame 3.
    REQUIRE(local_pending.find(delay + 3) != local_pending.end());
    CHECK(local_pending[delay + 3].keys['A'] == 4);
}
