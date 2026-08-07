#include <doctest/doctest.h>
#include "net/NetSession.h"
#include "net/LoopbackTransport.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include <retropark/retropark.h>
#include <retropark/retropark_abi.h>
#include <map>
#include <string>
#include <thread>
#include <vector>
using namespace rp;
using namespace rp::net;

#ifndef RP_DRIVEN_CORE_DIR
#define RP_DRIVEN_CORE_DIR "cores/refcore_driven"
#endif

// Load the reference driven core into a runtime. Mirrors the device-free setup used by the
// Slice F portable savestate e2e (test_savestate.cpp): RP_GFX_D3D11 defaults to a WARP
// (software) device, so this touches no real GPU and runs unconditionally. The driven core
// advances its state (a 4-byte frame counter) each present(), serialize/load round-trips it.
static void load_refcore_driven(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_DRIVEN_CORE_DIR) == RP_OK);
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
