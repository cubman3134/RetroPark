#include "net/RollbackSession.h"
#include "net/RollbackPredict.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <cstring>
namespace rp::net {
namespace {
rp_runtime* as_c(Runtime* r) { return reinterpret_cast<rp_runtime*>(r); }
void set_core_id(char (&dst)[64], const char* src) {          // no strncpy (C4996)
    std::memset(dst, 0, 64);
    if (src) { size_t n = std::strlen(src); if (n > 63) n = 63; std::memcpy(dst, src, n); }
}
}

rp_result RollbackSession::handshake(bool is_host, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err) {
    Hello mine{}; mine.abi_version = RETROPARK_ABI_VERSION; mine.content_hash = content_hash;
    mine.input_delay = max_prediction; mine.start_frame = 0;
    set_core_id(mine.core_id, core_id);
    auto bytes = encode_hello(mine);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "hello send failed"; return RP_ERR_DEVICE; }
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "hello recv failed"; return RP_ERR_DEVICE; }
    Hello peer{}; if (!decode_hello(in, peer)) { err = "bad hello"; return RP_ERR_INTERNAL; }
    if (peer.abi_version != RETROPARK_ABI_VERSION) { err = "abi mismatch"; return RP_ERR_ABI_MISMATCH; }
    if (peer.content_hash != content_hash || std::strncmp(peer.core_id, mine.core_id, 64) != 0) { err = "core/content mismatch"; return RP_ERR_BAD_ARG; }
    max_prediction_ = is_host ? max_prediction : peer.input_delay;   // host authoritative
    if (max_prediction_ == 0) max_prediction_ = 8;
    return RP_OK;
}

rp_result RollbackSession::start_host(Runtime& rt, ITransport& t, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 0; remote_port_ = 1;
    frame_ = 0; confirmed_ = 0; have_confirmed_ = false; verified_ = 0; rollback_count_ = 0; status_ = RbStatus::Ok;
    if (auto r = handshake(true, max_prediction, content_hash, core_id, err); r != RP_OK) return r;
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    StateSync s; s.frame = 0; s.blob.resize(sz);
    if (sz && rp_runtime_save_state(as_c(rt_), s.blob.data(), sz) != RP_OK) { err = "serialize failed"; return RP_ERR_INTERNAL; }
    auto bytes = encode_state_sync(s);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "state send failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result RollbackSession::start_join(Runtime& rt, ITransport& t, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 1; remote_port_ = 0;
    frame_ = 0; confirmed_ = 0; have_confirmed_ = false; verified_ = 0; rollback_count_ = 0; status_ = RbStatus::Ok;
    if (auto r = handshake(false, 0, content_hash, core_id, err); r != RP_OK) return r;
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "state recv failed"; return RP_ERR_DEVICE; }
    StateSync s; if (!decode_state_sync(in, s)) { err = "bad state sync"; return RP_ERR_INTERNAL; }
    if (!s.blob.empty() && rp_runtime_load_state(as_c(rt_), s.blob.data(), s.blob.size()) != RP_OK) { err = "load_state failed"; return RP_ERR_UNSUPPORTED; }
    return RP_OK;
}

void RollbackSession::save_ring(uint64_t f) {
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    std::vector<uint8_t> buf(sz);
    if (!sz || rp_runtime_save_state(as_c(rt_), buf.data(), sz) == RP_OK) ring_[f] = std::move(buf);
}

RbStatus RollbackSession::tick(const rp_input_state& local_now, uint8_t* out_rgba) {
    if (status_ == RbStatus::Disconnected) return status_;
    const uint64_t F = frame_;
    // 1. local input + send
    local_[F] = local_now;
    { Input m; m.frame = F; m.port = (uint8_t)local_port_; m.state = local_now;
      auto b = encode_input(m); if (t_->send(b.data(), b.size()) != RP_OK) { status_ = RbStatus::Disconnected; return status_; } }
    // 2. drain (non-blocking — rollback never waits on remote)
    for (;;) {
        std::vector<uint8_t> in; rp_result r = t_->recv(in, false, 0);
        if (r == RP_ERR_NOT_FOUND) break;
        if (r != RP_OK) { status_ = RbStatus::Disconnected; return status_; }
        MsgType ty; if (!peek_type(in, ty)) continue;
        if (ty == MsgType::Input)    { Input m;    if (decode_input(in, m))    { remote_[m.frame] = m.state; if (!have_confirmed_ || m.frame > confirmed_) { confirmed_ = m.frame; have_confirmed_ = true; } } }
        else if (ty == MsgType::Checksum) { Checksum c; if (decode_checksum(in, c)) peer_crc_[c.frame] = c.crc; }
    }
    // 3. reconcile [verified_, min(confirmed_, F-1)]
    if (have_confirmed_ && F > 0) {
        uint64_t hi = (confirmed_ < F - 1) ? confirmed_ : (F - 1);
        if (verified_ <= hi) {
            uint64_t g = rb_first_mispredicted(remote_, used_, verified_, hi);
            if (g != UINT64_MAX) {
                if (rp_runtime_load_state(as_c(rt_), ring_[g].data(), ring_[g].size()) != RP_OK) { status_ = RbStatus::Desync; return status_; }
                for (uint64_t f = g; f < F; ++f) {
                    rp_input_state rin = remote_.count(f) ? remote_[f] : rb_predict(remote_, confirmed_);
                    used_[f] = rin;
                    rp_runtime_set_input(as_c(rt_), local_port_, &local_[f]);
                    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
                    if (rp_runtime_advance(as_c(rt_), 0) != RP_OK) { status_ = RbStatus::Desync; return status_; }
                    save_ring(f + 1);
                }
                ++rollback_count_;
            }
            verified_ = hi + 1;
        }
    }
    // 4. stall guard — don't run further ahead than max_prediction_ unconfirmed frames
    uint64_t ahead = (F > 0 && have_confirmed_ && (F - 1) > confirmed_) ? (F - 1 - confirmed_) : 0;
    if (ahead >= max_prediction_) {
        rp_runtime_render(as_c(rt_), out_rgba);        // re-show last frame; don't advance
        maybe_send_checksum(); check_desync(); prune();
        status_ = RbStatus::Stalled; return status_;
    }
    // 5. simulate + display F
    save_ring(F);
    rp_input_state rin = remote_.count(F) ? remote_[F] : rb_predict(remote_, confirmed_);
    used_[F] = rin;
    rp_runtime_set_input(as_c(rt_), local_port_, &local_now);
    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
    if (rp_runtime_advance(as_c(rt_), 1) != RP_OK) { status_ = RbStatus::Desync; return status_; }
    rp_runtime_render(as_c(rt_), out_rgba);
    frame_ = F + 1;
    // 6. desync + prune
    maybe_send_checksum(); check_desync(); prune();
    status_ = RbStatus::Ok; return status_;
}

void RollbackSession::maybe_send_checksum() {
    if (verified_ == 0 || verified_ % kChecksumEvery != 0) return;
    auto it = ring_.find(verified_);
    if (it == ring_.end()) return;
    uint32_t crc = crc32(it->second.data(), it->second.size());
    own_crc_[verified_] = crc;
    Checksum c; c.frame = verified_; c.crc = crc;
    auto b = encode_checksum(c); t_->send(b.data(), b.size());
}
void RollbackSession::check_desync() {
    for (auto& kv : own_crc_) {
        auto pit = peer_crc_.find(kv.first);
        if (pit != peer_crc_.end() && pit->second != kv.second) { status_ = RbStatus::Desync; return; }
    }
}
void RollbackSession::prune() {
    uint64_t slack = max_prediction_ + 2;
    uint64_t floor = (verified_ > slack) ? (verified_ - slack) : 0;
    rb_prune_below(ring_, floor);
    rb_prune_below(local_, floor); rb_prune_below(remote_, floor); rb_prune_below(used_, floor);
}
} // namespace rp::net
