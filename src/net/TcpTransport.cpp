#include "net/TcpTransport.h"
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <cstring>
#include <mutex>
namespace rp::net {
namespace {
// Guarded by a mutex (not just an atomic counter) so that a concurrent caller
// blocks until WSAStartup() has actually completed, instead of racing ahead
// and calling socket()/etc. before the process-wide Winsock state is ready
// (which manifests as WSANOTINITIALISED even though the ref count looks sane).
std::mutex g_wsa_mutex;
int g_wsa_refs = 0;
bool wsa_startup() {
    std::lock_guard<std::mutex> lk(g_wsa_mutex);
    if (g_wsa_refs == 0) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
    }
    ++g_wsa_refs;
    return true;
}
void wsa_cleanup() {
    std::lock_guard<std::mutex> lk(g_wsa_mutex);
    if (--g_wsa_refs == 0) WSACleanup();
}
void set_nodelay(SOCKET s) { BOOL yes = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes)); }
} // namespace

rp_result TcpTransport::host(uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err, uint32_t accept_timeout_ms) {
    if (!wsa_startup()) { err = "WSAStartup failed"; return RP_ERR_DEVICE; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { err = "socket() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    BOOL reuse = TRUE; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { err = "bind() failed"; closesocket(listener); wsa_cleanup(); return RP_ERR_DEVICE; }
    if (listen(listener, 1) == SOCKET_ERROR) { err = "listen() failed"; closesocket(listener); wsa_cleanup(); return RP_ERR_DEVICE; }
    // bounded accept via select
    fd_set fds; FD_ZERO(&fds); FD_SET(listener, &fds);
    timeval tv{ (long)(accept_timeout_ms / 1000), (long)((accept_timeout_ms % 1000) * 1000) };
    int sel = select(0, &fds, nullptr, nullptr, &tv);
    if (sel <= 0) { err = "accept timeout"; closesocket(listener); wsa_cleanup(); return RP_ERR_TIMEOUT; }
    SOCKET peer = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (peer == INVALID_SOCKET) { err = "accept() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    set_nodelay(peer);
    out.reset(new TcpTransport(peer));   // owns the WSA ref taken above
    return RP_OK;
}
rp_result TcpTransport::join(const char* ip, uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err) {
    if (!wsa_startup()) { err = "WSAStartup failed"; return RP_ERR_DEVICE; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "socket() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { err = "bad ip"; closesocket(s); wsa_cleanup(); return RP_ERR_BAD_ARG; }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { err = "connect() failed"; closesocket(s); wsa_cleanup(); return RP_ERR_DEVICE; }
    set_nodelay(s);
    out.reset(new TcpTransport(s));
    return RP_OK;
}
TcpTransport::~TcpTransport() { close(); }
void TcpTransport::close() {
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; wsa_cleanup(); }
}
rp_result TcpTransport::send(const void* data, size_t size) {
    if (sock_ == INVALID_SOCKET) return RP_ERR_DEVICE;
    uint32_t len = uint32_t(size);
    uint8_t hdr[4] = { uint8_t(len), uint8_t(len>>8), uint8_t(len>>16), uint8_t(len>>24) };
    // send header then payload; loop to handle partial sends
    auto send_all = [&](const uint8_t* p, size_t n) -> bool {
        size_t sent = 0;
        while (sent < n) { int r = ::send(sock_, (const char*)p + sent, int(n - sent), 0); if (r <= 0) return false; sent += size_t(r); }
        return true;
    };
    if (!send_all(hdr, 4)) return RP_ERR_DEVICE;
    if (size && !send_all(static_cast<const uint8_t*>(data), size)) return RP_ERR_DEVICE;
    return RP_OK;
}
bool TcpTransport::recv_exact(uint8_t* buf, size_t n, uint32_t timeout_ms, rp_result& err) {
    DWORD tv = timeout_ms; setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    size_t got = 0;
    while (got < n) {
        int r = ::recv(sock_, (char*)buf + got, int(n - got), 0);
        if (r == 0) { err = RP_ERR_DEVICE; return false; }             // peer closed
        if (r == SOCKET_ERROR) { err = (WSAGetLastError() == WSAETIMEDOUT) ? RP_ERR_TIMEOUT : RP_ERR_DEVICE; return false; }
        got += size_t(r);
    }
    return true;
}
rp_result TcpTransport::recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) {
    if (sock_ == INVALID_SOCKET) return RP_ERR_DEVICE;
    if (!block) {
        // poll: is there at least one byte ready?
        fd_set fds; FD_ZERO(&fds); FD_SET(sock_, &fds); timeval tv{0,0};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return RP_ERR_NOT_FOUND;
        timeout_ms = 2000;   // a frame is pending; read it fully with a safety timeout
    }
    uint8_t hdr[4]; rp_result err;
    if (!recv_exact(hdr, 4, timeout_ms, err)) return err;
    uint32_t len = uint32_t(hdr[0]) | uint32_t(hdr[1])<<8 | uint32_t(hdr[2])<<16 | uint32_t(hdr[3])<<24;
    out.resize(len);
    if (len && !recv_exact(out.data(), len, timeout_ms, err)) return err;
    return RP_OK;
}
} // namespace rp::net
