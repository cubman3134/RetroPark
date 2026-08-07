#pragma once
#include "net/ITransport.h"
#include <string>
#include <memory>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
namespace rp::net {

// Blocking, reliable, ordered 1:1 TCP channel. Framing: uint32 LE payload length, then payload.
class TcpTransport : public ITransport {
public:
    static rp_result host(uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err, uint32_t accept_timeout_ms);
    static rp_result join(const char* ip, uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err);
    ~TcpTransport() override;
    rp_result send(const void* data, size_t size) override;
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) override;
    bool connected() const override { return sock_ != INVALID_SOCKET; }
    void close() override;
private:
    explicit TcpTransport(SOCKET s) : sock_(s) {}
    bool recv_exact(uint8_t* buf, size_t n, uint32_t timeout_ms, rp_result& err);
    SOCKET sock_ = INVALID_SOCKET;
};
} // namespace rp::net
