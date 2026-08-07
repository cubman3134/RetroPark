#include "net/LoopbackTransport.h"
#include <chrono>
namespace rp::net {

rp_result LoopbackTransport::send(const void* data, size_t size) {
    auto* ch = out_.get();
    std::lock_guard<std::mutex> lk(ch->m);
    if (!ch->open) return RP_ERR_DEVICE;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    ch->q.emplace_back(p, p + size);
    ch->cv.notify_one();
    return RP_OK;
}
rp_result LoopbackTransport::recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) {
    auto* ch = in_.get();
    std::unique_lock<std::mutex> lk(ch->m);
    if (ch->q.empty()) {
        if (!block) return RP_ERR_NOT_FOUND;
        ch->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return !ch->q.empty() || !ch->open; });
        if (ch->q.empty()) return ch->open ? RP_ERR_TIMEOUT : RP_ERR_DEVICE;
    }
    out = std::move(ch->q.front());
    ch->q.pop_front();
    return RP_OK;
}
bool LoopbackTransport::connected() const {
    std::lock_guard<std::mutex> lk(out_->m);
    return out_->open;
}
void LoopbackTransport::close() {
    { std::lock_guard<std::mutex> lk(out_->m); out_->open = false; out_->cv.notify_all(); }
    { std::lock_guard<std::mutex> lk(in_->m);  in_->open  = false; in_->cv.notify_all(); }
}
std::pair<std::shared_ptr<LoopbackTransport>, std::shared_ptr<LoopbackTransport>> make_loopback_pair() {
    auto a2b = std::make_shared<LoopbackTransport::Channel>();
    auto b2a = std::make_shared<LoopbackTransport::Channel>();
    auto a = std::make_shared<LoopbackTransport>(/*in=*/b2a, /*out=*/a2b);
    auto b = std::make_shared<LoopbackTransport>(/*in=*/a2b, /*out=*/b2a);
    return {a, b};
}
} // namespace rp::net
