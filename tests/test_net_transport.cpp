#include <doctest/doctest.h>
#include "net/LoopbackTransport.h"
#include <thread>
using namespace rp::net;

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
