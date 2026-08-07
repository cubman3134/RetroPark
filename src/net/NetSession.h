#pragma once
#include "net/ITransport.h"
#include "net/NetProtocol.h"
#include "retropark/retropark_abi.h"
#include <map>
#include <string>
namespace rp { class Runtime; }
namespace rp::net {

enum class NetStatus { Ok, Waiting, Desync, Disconnected };

class NetSession {
public:
    rp_result start_host(Runtime& rt, ITransport& t, uint32_t input_delay,
                         uint64_t content_hash, const char* core_id, std::string& err);
    rp_result start_join(Runtime& rt, ITransport& t,
                         uint64_t content_hash, const char* core_id, std::string& err);
    void      tick_send(const rp_input_state& local_now);
    NetStatus tick_recv_and_advance();
    NetStatus tick(const rp_input_state& local_now) { tick_send(local_now); return tick_recv_and_advance(); }
    uint64_t  frame() const { return frame_; }
    NetStatus status() const { return status_; }

private:
    rp_result handshake(bool is_host, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err);
    Runtime*    rt_ = nullptr;
    ITransport* t_  = nullptr;
    uint32_t    local_port_ = 0, remote_port_ = 1;
    uint32_t    delay_ = 0;
    uint64_t    frame_ = 0;
    NetStatus   status_ = NetStatus::Ok;
    std::map<uint64_t, rp_input_state> local_pending_;   // by apply-frame
    std::map<uint64_t, rp_input_state> remote_pending_;  // by apply-frame
    std::map<uint64_t, uint32_t>       own_crc_, peer_crc_;
    static constexpr uint64_t kChecksumEvery = 60;
    static constexpr uint32_t kRecvTimeoutMs = 2000;
};
} // namespace rp::net
