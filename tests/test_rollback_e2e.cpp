#include <doctest/doctest.h>
#include "net/RollbackSession.h"
#include "net/LoopbackTransport.h"
#include "net/NetProtocol.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <deque>
#include <memory>
#include <thread>
#include <vector>
#include <string>
using namespace rp;
using namespace rp::net;

#ifndef RP_ROLLBACK_CORE_DIR
#define RP_ROLLBACK_CORE_DIR "cores/refcore_rollback"
#endif

// Load refcore_rollback into a runtime (self-contained copy of the Task-2 helper in
// test_rollback_unit.cpp, so this e2e TU needs no cross-file sharing).
static void load_refcore_rollback_e2e(Runtime& rt) {
    REQUIRE(rt.resize(64, 64) == RP_OK);
    REQUIRE(rt.load_core(RP_ROLLBACK_CORE_DIR) == RP_OK);
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
