#pragma once
#include "net/ITransport.h"
#include "net/NetProtocol.h"
#include "retropark/retropark_abi.h"
#include <map>
#include <vector>
#include <string>
namespace rp { class Runtime; }
namespace rp::net {

enum class RbStatus { Ok, Stalled, Desync, Disconnected };

class RollbackSession {
public:
    rp_result start_host(Runtime& rt, ITransport& t, uint32_t max_prediction,
                         uint64_t content_hash, const char* core_id, std::string& err);
    rp_result start_join(Runtime& rt, ITransport& t,
                         uint64_t content_hash, const char* core_id, std::string& err);
    // Simulate + display one frame. local_now = local input this frame; out_rgba receives the frame.
    RbStatus tick(const rp_input_state& local_now, uint8_t* out_rgba);
    uint64_t frame() const { return frame_; }
    uint64_t confirmed_frame() const { return confirmed_; }
    uint64_t rollback_count() const { return rollback_count_; }
    RbStatus status() const { return status_; }

private:
    rp_result handshake(bool is_host, uint32_t max_prediction, uint64_t content_hash, const char* core_id, std::string& err);
    void save_ring(uint64_t f);
    void maybe_send_checksum();
    void check_desync();
    void prune();

    Runtime*    rt_ = nullptr;
    ITransport* t_  = nullptr;
    uint32_t    local_port_ = 0, remote_port_ = 1;
    uint32_t    max_prediction_ = 8;
    uint64_t    frame_ = 0;          // next frame to simulate ([0,frame_) simulated)
    uint64_t    confirmed_ = 0;      // highest frame with a real remote input
    bool        have_confirmed_ = false;
    uint64_t    verified_ = 0;       // frames [0,verified_) reconciled with real remote input
    uint64_t    rollback_count_ = 0;
    RbStatus    status_ = RbStatus::Ok;
    std::map<uint64_t, std::vector<uint8_t>> ring_;   // frame -> pre-frame serialized state
    std::map<uint64_t, rp_input_state> local_, remote_, used_;
    std::map<uint64_t, uint32_t> own_crc_, peer_crc_;
    static constexpr uint64_t kChecksumEvery = 60;
    static constexpr uint32_t kRecvTimeoutMs = 2000;
};
} // namespace rp::net
