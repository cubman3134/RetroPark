#pragma once
#include "net/ITransport.h"
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <utility>
namespace rp::net {

// A pair of endpoints sharing two message queues (A->B and B->A). Message boundaries are
// preserved: one send == one queue entry == one recv. Thread-safe.
class LoopbackTransport : public ITransport {
public:
    struct Channel { std::mutex m; std::condition_variable cv; std::deque<std::vector<uint8_t>> q; bool open = true; };
    LoopbackTransport(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out) : in_(std::move(in)), out_(std::move(out)) {}
    rp_result send(const void* data, size_t size) override;
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) override;
    bool connected() const override;
    void close() override;
private:
    std::shared_ptr<Channel> in_, out_;
};

std::pair<std::shared_ptr<LoopbackTransport>, std::shared_ptr<LoopbackTransport>> make_loopback_pair();
} // namespace rp::net
