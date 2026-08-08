#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <doctest/doctest.h>
#include "net/RollbackSession.h"
#include "net/LoopbackTransport.h"
#include "net/NetProtocol.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>
#include <string>
using namespace rp;
using namespace rp::net;

#ifndef RP_ROLLBACK_CORE_DIR
#define RP_ROLLBACK_CORE_DIR "cores/refcore_rollback"
#endif
#ifndef RP_SHIM_DIR
#define RP_SHIM_DIR "cores/libretro_shim"
#endif
#ifndef RP_NES_ROM_DIR
#define RP_NES_ROM_DIR "C:/RetroBat/roms/nes"
#endif

// Load refcore_rollback into a runtime (self-contained copy of the Task-2 helper in
// test_rollback_unit.cpp, so this e2e TU needs no cross-file sharing).
static void load_refcore_rollback_e2e(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_ROLLBACK_CORE_DIR) == RP_OK);
}

// ---- Gated FCEUmm setup (lifted from tests/test_netplay_e2e.cpp, Slice G) ----
//
// Duplicated (not shared) probe helpers, matching the sibling gated e2e files
// (test_libretro_e2e.cpp, test_savestate.cpp, test_netplay_e2e.cpp), which each keep their own
// prefixed static copy rather than a cross-TU header for a few tiny functions.
static bool rollback_file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static std::string rollback_first_nes(const std::string& dir) {
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
    return !rollback_first_nes(RP_NES_ROM_DIR).empty() &&
           rollback_file_exists(std::string(RP_SHIM_DIR) + "/fceumm_libretro.dll");
}

// FCEUmm keeps its CPU/PPU/APU state in process-wide C globals, and the shim's own instance
// pointer is one-per-loaded-module too. LoadLibrary is refcounted BY PATH: two Runtimes that
// both load the same on-disk shim DLL get back the SAME Windows module and would silently
// share one emulator's memory instead of being genuinely independent runtimes. Give each call
// its own on-disk copy of the whole shim package (shim + wrapped core + manifest) so
// Win32CoreModule::open() sees a distinct file per side and gets a separate module image
// (separate statics) from Windows. (Slice G idiom, copied verbatim.)
static std::string rollback_private_shim_copy() {
    namespace fs = std::filesystem;
    static std::atomic<int> counter{0};
    int id = counter.fetch_add(1);
    std::error_code ec;
    fs::path dst = fs::temp_directory_path(ec) / ("rp_rollback_shim_" + std::to_string(id));
    fs::remove_all(dst, ec);
    fs::create_directories(dst, ec);
    for (const auto& entry : fs::directory_iterator(RP_SHIM_DIR, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        fs::copy_file(entry.path(), dst / entry.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    return dst.string();
}

// Load the real libretro shim + a real Donkey Kong (first .nes found) into rt, from a private
// per-call copy of the shim package so two runtimes loaded in this one process never alias the
// same DLL image.
static void load_shim_with_donkey_kong(Runtime& rt) {
    std::string rom = rollback_first_nes(RP_NES_ROM_DIR);
    REQUIRE(!rom.empty());
    std::string shim_dir = rollback_private_shim_copy();
    REQUIRE(rt.resize(256, 240) == RP_OK);          // NES resolution
    REQUIRE(rt.load_core(shim_dir) == RP_OK);
    REQUIRE(rp_runtime_load_content(reinterpret_cast<rp_runtime*>(&rt), rom.c_str()) == RP_OK);
}

// Test transport: wraps a loopback endpoint; INPUT/CHECKSUM messages are held `delay` clock-ticks;
// HELLO/STATE_SYNC pass through immediately so the handshake isn't stalled. tick_clock() releases.
class DelayTransport : public ITransport {
public:
    DelayTransport(std::shared_ptr<ITransport> inner, uint32_t delay) : inner_(std::move(inner)), delay_(delay) {}
    rp_result send(const void* d, size_t n) override { return inner_->send(d, n); }
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t tmo) override {
        drain();
        if (block) {                                   // handshake path: wait for a control msg
            while (staged_.empty() || staged_.front().first > now_) {
                std::vector<uint8_t> m;
                rp_result r = inner_->recv(m, true, tmo);
                if (r != RP_OK) return r;
                stage(m);
                drain_nonblock();
            }
        }
        if (!staged_.empty() && staged_.front().first <= now_) { out = staged_.front().second; staged_.pop_front(); return RP_OK; }
        return RP_ERR_NOT_FOUND;
    }
    bool connected() const override { return inner_->connected(); }
    void close() override { inner_->close(); }
    void tick_clock() { ++now_; }
private:
    void drain_nonblock() { std::vector<uint8_t> m; while (inner_->recv(m, false, 0) == RP_OK) stage(m); }
    void drain() { drain_nonblock(); }
    void stage(const std::vector<uint8_t>& m) {
        MsgType ty; uint64_t release = now_;
        if (peek_type(m, ty) && (ty == MsgType::Input || ty == MsgType::Checksum)) release = now_ + delay_;
        staged_.emplace_back(release, m);
    }
    std::shared_ptr<ITransport> inner_;
    uint32_t delay_;
    uint64_t now_ = 0;
    std::deque<std::pair<uint64_t, std::vector<uint8_t>>> staged_;
};

TEST_CASE("rollback: mispredictions roll back and converge to lockstep ground truth (portable)") {
    const int N = 120;
    const int kFlush = 20;
    // Fixed 2-port input plan (port0 = host/A, port1 = join/B); both known for all frames.
    auto A = [](int f){ rp_input_state s{}; s.keys['X'] = (f % 4 == 0); return s; };
    auto B = [](int f){ rp_input_state s{}; s.keys['X'] = (f % 3 == 0); return s; };

    // Ground truth: one runtime, feed both ports directly, record acc each frame. Covers the
    // main loop (frames 0..N-1, inputs A(f)/B(f)) AND the flush window (frames N..N+kFlush-1,
    // which repeats A(N-1)/B(N-1)) so truth[] has an entry for every frame either session
    // simulates, including the trailing flush.
    std::vector<uint32_t> truth(N + kFlush + 1);
    {
        Runtime g(RP_GFX_D3D11, nullptr); load_refcore_rollback_e2e(g);
        auto rt = reinterpret_cast<rp_runtime*>(&g);
        auto acc = [&]{ uint32_t a=0; rp_runtime_save_state(rt,&a,sizeof(a)); return a; };
        truth[0] = acc();
        std::vector<uint8_t> out(64*64*4);
        for (int f = 0; f < N + kFlush; ++f) {
            rp_input_state a = (f < N) ? A(f) : A(N-1);
            rp_input_state b = (f < N) ? B(f) : B(N-1);
            rp_runtime_set_input(rt, 0, &a); rp_runtime_set_input(rt, 1, &b);
            rp_runtime_advance(rt, 1); rp_runtime_render(rt, out.data());
            truth[f+1] = acc();
        }
    }

    // Rollback run: two sessions over DelayTransport (remote inputs 3 clock-ticks late).
    Runtime rh(RP_GFX_D3D11, nullptr), rj(RP_GFX_D3D11, nullptr);
    load_refcore_rollback_e2e(rh); load_refcore_rollback_e2e(rj);
    auto [la, lb] = make_loopback_pair();
    auto dh = std::make_shared<DelayTransport>(la, 3);
    auto dj = std::make_shared<DelayTransport>(lb, 3);
    RollbackSession sh, sj; std::string err;
    // symmetric handshake blocks -> run host on a thread (Slice G idiom)
    std::thread th([&]{ REQUIRE(sh.start_host(rh, *dh, /*max_pred=*/8, /*hash=*/0, "refcore_rollback", err) == RP_OK); });
    REQUIRE(sj.start_join(rj, *dj, /*hash=*/0, "refcore_rollback", err) == RP_OK);
    th.join();

    std::vector<uint8_t> oh(64*64*4), oj(64*64*4);
    for (int f = 0; f < N; ++f) {
        rp_input_state a = A(f), b = B(f);
        sh.tick(a, oh.data());
        sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();          // release delayed messages one tick later
    }
    // Flush: keep ticking (repeating the last input) + advancing clocks until both fully reconciled.
    for (int f = N; f < N + kFlush; ++f) {
        rp_input_state a = A(N-1), b = B(N-1);
        sh.tick(a, oh.data()); sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();
    }
    CHECK(sh.rollback_count() > 0);                   // mispredictions really happened
    CHECK(sj.rollback_count() > 0);
    CHECK(sh.status() != RbStatus::Desync);
    CHECK(sj.status() != RbStatus::Desync);
    // Anchor convergence to the INDEPENDENTLY-computed ground truth (not just peer-vs-peer):
    // frame() is "next frame to simulate", i.e. [0, frame()) has been simulated, so the live
    // runtime state after frame() frames matches truth[frame()] exactly (the flush window ran
    // long enough, >max_prediction, that no stall ever holds frame() back).
    auto acc_of = [](Runtime& r){ uint32_t a=0; rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&r),&a,sizeof(a)); return a; };
    uint64_t fh = sh.frame(), fj = sj.frame();
    REQUIRE(fh == (uint64_t)(N + kFlush));
    REQUIRE(fj == (uint64_t)(N + kFlush));
    CHECK(acc_of(rh) == truth[fh]);                   // host reconciled state == ground truth
    CHECK(acc_of(rj) == truth[fj]);                   // join reconciled state == ground truth
    CHECK(acc_of(rh) == acc_of(rj));                  // peers agree (lockstep-equivalent)
}

// ---- Gated: two real FCEUmm/Donkey Kong runtimes converge under delay+rollback ----
//
// The portable gate above proves the RollbackSession contract (predict/rollback/reconcile)
// against the deterministic refcore_rollback stub. This gate proves the same guarantee
// against a real emulator core and a real ROM: two independently loaded FCEUmm runtimes,
// advanced identically past boot, then driven only through RollbackSession's handshake +
// per-frame rollback tick over a delayed transport, converge (byte-identical full savestate
// CRC) despite genuine mispredictions and rollbacks along the way.
// Gated: WARN-skip (not fail) if the shim core DLL or a NES ROM is absent on this machine.
TEST_CASE("rollback: two FCEUmm runtimes converge under delay (gated)") {
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping rollback FCEUmm gate"); return; }
    Runtime rh(RP_GFX_D3D11, nullptr), rj(RP_GFX_D3D11, nullptr);
    load_shim_with_donkey_kong(rh); load_shim_with_donkey_kong(rj);   // per-instance shim copy (Slice G idiom)
    // advance both past boot identically before sync
    for (int i = 0; i < 200; ++i) { rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rh), 1);
                                    rp_runtime_advance(reinterpret_cast<rp_runtime*>(&rj), 1); }

    auto [la, lb] = make_loopback_pair();
    auto dh = std::make_shared<DelayTransport>(la, 3), dj = std::make_shared<DelayTransport>(lb, 3);
    RollbackSession sh, sj; std::string err;
    std::thread th([&]{ REQUIRE(sh.start_host(rh, *dh, 8, 0xD0, "fceumm", err) == RP_OK); });
    REQUIRE(sj.start_join(rj, *dj, 0xD0, "fceumm", err) == RP_OK);
    th.join();

    auto crc_of = [](Runtime& r){ size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&r));
        std::vector<uint8_t> b(sz); rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&r), b.data(), sz);
        return rp::net::crc32(b.data(), sz); };
    REQUIRE(crc_of(rh) == crc_of(rj));                 // state sync aligned them

    std::vector<uint8_t> oh(256*240*4), oj(256*240*4);
    for (int f = 0; f < 120; ++f) {
        rp_input_state a{}, b{}; a.keys[VK_RIGHT] = (f % 2 == 0); b.keys['X'] = (f % 5 == 0);
        sh.tick(a, oh.data()); sj.tick(b, oj.data());
        dh->tick_clock(); dj->tick_clock();
    }
    for (int f = 0; f < 20; ++f) { rp_input_state a{}, b{}; sh.tick(a, oh.data()); sj.tick(b, oj.data()); dh->tick_clock(); dj->tick_clock(); }
    CHECK(sh.rollback_count() > 0);
    CHECK(sh.status() != RbStatus::Desync);
    CHECK(crc_of(rh) == crc_of(rj));                   // real NES converged under delay+rollback
}

// ---- Gated: emit_audio=0 suppresses audio during silent rollback re-simulation ----
//
// RollbackSession re-simulates rolled-back frames with rp_runtime_advance(rt, 0) (step 3 of
// tick()) so the audio the emulator would have produced the FIRST time a frame ran isn't
// duplicated when that frame is replayed after a misprediction. This proves the underlying
// advance()/audio_stats() contract directly: audio frame count is flat while emit_audio=0,
// and resumes growing once emit_audio=1 again.
TEST_CASE("rollback: advance(emit_audio=0) suppresses audio (gated)") {
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping audio-suppression check"); return; }
    Runtime r(RP_GFX_D3D11, nullptr); load_shim_with_donkey_kong(r);
    auto rt = reinterpret_cast<rp_runtime*>(&r);
    for (int i = 0; i < 60; ++i) rp_runtime_advance(rt, 1);        // past boot, audio flowing
    uint64_t f0 = 0; int ns0 = 0; rp_runtime_audio_stats(rt, &f0, &ns0);
    for (int i = 0; i < 60; ++i) rp_runtime_advance(rt, 0);        // silent re-sim
    uint64_t f1 = 0; int ns1 = 0; rp_runtime_audio_stats(rt, &f1, &ns1);
    CHECK(f1 == f0);                                              // no audio counted during emit_audio=0
    for (int i = 0; i < 10; ++i) rp_runtime_advance(rt, 1);
    uint64_t f2 = 0; int ns2 = 0; rp_runtime_audio_stats(rt, &f2, &ns2);
    CHECK(f2 > f1);                                               // audio resumes with emit_audio=1
}
