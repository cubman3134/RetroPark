#include "net/NetSession.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include <retropark/retropark.h>
#include <cstring>
#include <vector>
namespace rp::net {
namespace { rp_runtime* as_c(Runtime* r) { return reinterpret_cast<rp_runtime*>(r); } }

// Bounded copy into the fixed 64-byte, null-padded Hello::core_id. NOT strncpy: strncpy is
// C4996-deprecated under MSVC /W4 and any warning fails this slice's gate. `mine.core_id` is
// already zero-initialized by `Hello mine{}`, so copying min(strlen(src), 63) bytes leaves
// the remainder (incl. core_id[63]) as the guaranteed null terminator.
static void set_core_id(char (&dst)[64], const char* src) {
    if (!src) return;
    size_t n = std::strlen(src);
    if (n > sizeof(dst) - 1) n = sizeof(dst) - 1;
    std::memcpy(dst, src, n);
}

rp_result NetSession::handshake(bool is_host, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err) {
    Hello mine{}; mine.abi_version = RETROPARK_ABI_VERSION; mine.content_hash = content_hash;
    mine.input_delay = input_delay; mine.start_frame = 0;
    set_core_id(mine.core_id, core_id ? core_id : "");
    auto bytes = encode_hello(mine);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "hello send failed"; return RP_ERR_DEVICE; }
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "hello recv failed"; return RP_ERR_DEVICE; }
    Hello peer{}; if (!decode_hello(in, peer)) { err = "bad hello"; return RP_ERR_INTERNAL; }
    if (peer.abi_version != RETROPARK_ABI_VERSION) { err = "abi mismatch"; return RP_ERR_ABI_MISMATCH; }
    if (peer.content_hash != content_hash || std::memcmp(peer.core_id, mine.core_id, sizeof(mine.core_id)) != 0) { err = "core/content mismatch"; return RP_ERR_BAD_ARG; }
    // The host's input_delay is authoritative: the host uses its own arg; the join adopts the
    // delay carried in the host's Hello so both sides run the identical apply-frame schedule.
    delay_ = is_host ? input_delay : peer.input_delay;
    return RP_OK;
}

rp_result NetSession::start_host(Runtime& rt, ITransport& t, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 0; remote_port_ = 1; frame_ = 0; status_ = NetStatus::Ok;
    if (auto r = handshake(true, input_delay, content_hash, core_id, err); r != RP_OK) return r;
    // authoritative initial state -> STATE_SYNC
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    StateSync s; s.frame = 0; s.blob.resize(sz);
    if (sz && rp_runtime_save_state(as_c(rt_), s.blob.data(), sz) != RP_OK) { err = "serialize failed"; return RP_ERR_INTERNAL; }
    auto bytes = encode_state_sync(s);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "state send failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result NetSession::start_join(Runtime& rt, ITransport& t, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 1; remote_port_ = 0; frame_ = 0; status_ = NetStatus::Ok;
    // delay is learned from the host's Hello inside handshake() (the 0 here is ignored for join).
    if (auto r = handshake(false, /*input_delay=*/0, content_hash, core_id, err); r != RP_OK) return r;
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "state recv failed"; return RP_ERR_DEVICE; }
    StateSync s; if (!decode_state_sync(in, s)) { err = "bad state sync"; return RP_ERR_INTERNAL; }
    if (!s.blob.empty() && rp_runtime_load_state(as_c(rt_), s.blob.data(), s.blob.size()) != RP_OK) { err = "load_state failed"; return RP_ERR_UNSUPPORTED; }
    return RP_OK;
}

void NetSession::tick_send(const rp_input_state& local_now) {
    if (status_ == NetStatus::Disconnected) return;
    uint64_t apply = frame_ + delay_;
    local_pending_[apply] = local_now;
    Input msg; msg.frame = apply; msg.port = uint8_t(local_port_); msg.state = local_now;
    auto bytes = encode_input(msg);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) status_ = NetStatus::Disconnected;
}

NetStatus NetSession::tick_recv_and_advance() {
    if (status_ == NetStatus::Disconnected) return status_;
    // The remote sent nothing for absolute frames [0, delay_): both machines apply the zeroed
    // neutral input on both ports at those frames (consistent). Only for frame_ >= delay_ is a
    // remote input owed — drain until it arrives (Input -> remote_pending_, Checksum -> peer_crc_).
    if (frame_ >= delay_) {
        while (remote_pending_.find(frame_) == remote_pending_.end()) {
            std::vector<uint8_t> in;
            rp_result r = t_->recv(in, true, kRecvTimeoutMs);
            if (r == RP_ERR_TIMEOUT) { status_ = NetStatus::Waiting; return status_; }
            if (r != RP_OK) { status_ = NetStatus::Disconnected; return status_; }
            MsgType ty; if (!peek_type(in, ty)) continue;
            if (ty == MsgType::Input)         { Input m;    if (decode_input(in, m))    remote_pending_[m.frame] = m.state; }
            else if (ty == MsgType::Checksum) { Checksum c; if (decode_checksum(in, c)) peer_crc_[c.frame] = c.crc; }
        }
    }
    // Apply the input PAIR destined for this absolute frame (neutral where none was sent).
    rp_input_state neutral{};
    auto lit = local_pending_.find(frame_);
    const rp_input_state& lin = (lit != local_pending_.end()) ? lit->second : neutral;
    auto rit = remote_pending_.find(frame_);
    const rp_input_state& rin = (rit != remote_pending_.end()) ? rit->second : neutral;
    rp_runtime_set_input(as_c(rt_), local_port_, &lin);
    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
    rp_runtime_present(as_c(rt_), nullptr);     // headless advance; the driven path accepts null readback

    // Periodic desync checksum: crc32 of the serialized state AFTER presenting this frame.
    if (frame_ % kChecksumEvery == 0) {
        size_t sz = rp_runtime_serialize_size(as_c(rt_));
        std::vector<uint8_t> buf(sz);
        if (sz == 0 || rp_runtime_save_state(as_c(rt_), buf.data(), sz) == RP_OK) {
            uint32_t own = crc32(buf.data(), sz);
            own_crc_[frame_] = own;
            Checksum c; c.frame = frame_; c.crc = own;
            auto bytes = encode_checksum(c);
            if (t_->send(bytes.data(), bytes.size()) != RP_OK) { status_ = NetStatus::Disconnected; return status_; }
        }
    }
    // Compare every frame for which both crcs are known; any mismatch is a hard desync.
    for (auto& kv : own_crc_) {
        auto pit = peer_crc_.find(kv.first);
        if (pit != peer_crc_.end() && pit->second != kv.second) { status_ = NetStatus::Desync; return status_; }
    }
    // Prune the input rings (never need a frame below the current one again) and, on a much
    // wider window, the crc maps (past the checksum round-trip) so nothing grows unbounded.
    uint64_t lo = (frame_ >= 3) ? frame_ - 3 : 0;
    for (auto it = remote_pending_.begin(); it != remote_pending_.end() && it->first < lo; ) it = remote_pending_.erase(it);
    for (auto it = local_pending_.begin();  it != local_pending_.end()  && it->first < lo; ) it = local_pending_.erase(it);
    uint64_t clo = (frame_ >= 2 * kChecksumEvery) ? frame_ - 2 * kChecksumEvery : 0;
    for (auto it = own_crc_.begin();  it != own_crc_.end()  && it->first < clo; ) it = own_crc_.erase(it);
    for (auto it = peer_crc_.begin(); it != peer_crc_.end() && it->first < clo; ) it = peer_crc_.erase(it);

    ++frame_;
    status_ = NetStatus::Ok;
    return status_;
}
} // namespace rp::net
