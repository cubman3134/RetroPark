#include <doctest/doctest.h>
#include "net/LoopbackTransport.h"
#include <thread>
using namespace rp::net;

#include "net/TcpTransport.h"
#include <atomic>

TEST_CASE("net: tcp localhost round-trip (framing + partial reads)") {
    const uint16_t port = 47654;
    std::unique_ptr<TcpTransport> server, client;
    std::string herr, jerr;
    std::atomic<bool> accepted{false};
    // host() accepts on a thread; join() connects from the test thread.
    std::thread t([&]{
        REQUIRE(TcpTransport::host(port, server, herr, 3000) == RP_OK);
        accepted = true;
    });
    // brief spin until the listener is up, then connect
    rp_result jr = RP_ERR_DEVICE;
    for (int i = 0; i < 50 && jr != RP_OK; ++i) {
        jr = TcpTransport::join("127.0.0.1", port, client, jerr);
        if (jr != RP_OK) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(jr == RP_OK);
    t.join();
    REQUIRE(accepted.load());

    // small message client -> server
    std::vector<uint8_t> small{1,2,3,4,5};
    CHECK(client->send(small.data(), small.size()) == RP_OK);
    std::vector<uint8_t> got;
    REQUIRE(server->recv(got, true, 2000) == RP_OK);
    CHECK(got == small);

    // multi-KB blob server -> client (exercises partial reads)
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 31 + 7);
    CHECK(server->send(big.data(), big.size()) == RP_OK);
    REQUIRE(client->recv(got, true, 2000) == RP_OK);
    CHECK(got == big);
}

TEST_CASE("net: loopback carries framed messages both directions") {
    auto [a, b] = make_loopback_pair();
    std::vector<uint8_t> m1{1,2,3}, m2{9,8,7,6};
    CHECK(a->send(m1.data(), m1.size()) == RP_OK);
    CHECK(a->send(m2.data(), m2.size()) == RP_OK);
    std::vector<uint8_t> got;
    REQUIRE(b->recv(got, true, 1000) == RP_OK); CHECK(got == m1);   // boundaries preserved
    REQUIRE(b->recv(got, true, 1000) == RP_OK); CHECK(got == m2);
    // reverse direction
    std::vector<uint8_t> r{42};
    CHECK(b->send(r.data(), r.size()) == RP_OK);
    REQUIRE(a->recv(got, true, 1000) == RP_OK); CHECK(got == r);
}

TEST_CASE("net: loopback non-blocking poll returns NOT_FOUND when empty") {
    auto [a, b] = make_loopback_pair();
    std::vector<uint8_t> got;
    CHECK(b->recv(got, false, 0) == RP_ERR_NOT_FOUND);
}

TEST_CASE("net: loopback close disconnects peer") {
    auto [a, b] = make_loopback_pair();
    a->close();
    std::vector<uint8_t> got;
    CHECK(b->recv(got, true, 100) == RP_ERR_DEVICE);   // peer closed, nothing queued
    CHECK(a->connected() == false);
}
