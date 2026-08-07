#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "retropark/retropark_abi.h"   // rp_result
namespace rp::net {
struct ITransport {
    virtual ~ITransport() = default;
    // Send one complete message (the transport frames it). RP_ERR_* on disconnect/failure.
    virtual rp_result send(const void* data, size_t size) = 0;
    // Receive one complete message. block=true waits up to timeout_ms (RP_ERR_TIMEOUT on expiry);
    // block=false polls (RP_ERR_NOT_FOUND if nothing ready). Peer-closed -> RP_ERR_DEVICE.
    virtual rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) = 0;
    virtual bool connected() const = 0;
    virtual void close() = 0;
};
} // namespace rp::net
