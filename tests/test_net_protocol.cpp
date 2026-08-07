#include <doctest/doctest.h>
#include "net/Crc32.h"
#include "net/NetProtocol.h"
#include <cstring>
#include <string>
using namespace rp::net;

TEST_CASE("net: crc32 known vector") {
    // "123456789" -> 0xCBF43926 (standard CRC-32 check value)
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0u);
}

TEST_CASE("net: hello round-trips") {
    Hello h; h.abi_version = 5; h.content_hash = 0xDEADBEEFCAFEULL;
    h.input_delay = 3; h.start_frame = 100;
    std::strncpy(h.core_id, "fceumm", sizeof(h.core_id) - 1);
    auto bytes = encode_hello(h);
    MsgType t; REQUIRE(peek_type(bytes, t)); CHECK(t == MsgType::Hello);
    Hello g; REQUIRE(decode_hello(bytes, g));
    CHECK(g.abi_version == 5u);
    CHECK(g.content_hash == 0xDEADBEEFCAFEULL);
    CHECK(g.input_delay == 3u);
    CHECK(g.start_frame == 100u);
    CHECK(std::string(g.core_id) == "fceumm");
}

TEST_CASE("net: input round-trips incl. signed axis, little-endian on the wire") {
    Input in; in.frame = 42; in.port = 1;
    in.state.keys[88] = 1;              // 'X'
    in.state.pad_axes[0] = -12345;      // signed
    in.state.pad_buttons = 0xBEEF;
    auto bytes = encode_input(in);
    MsgType t; REQUIRE(peek_type(bytes, t)); CHECK(t == MsgType::Input);
    Input g; REQUIRE(decode_input(bytes, g));
    CHECK(g.frame == 42u);
    CHECK(g.port == 1);
    CHECK(g.state.keys[88] == 1);
    CHECK(g.state.pad_axes[0] == -12345);
    CHECK(g.state.pad_buttons == 0xBEEFu);
    // frame field is bytes [1..8] little-endian: low byte first
    CHECK(bytes[1] == 42);
    CHECK(bytes[2] == 0);
}

TEST_CASE("net: state_sync round-trips a blob") {
    StateSync s; s.frame = 7; s.blob = {1, 2, 3, 250, 0, 99};
    auto bytes = encode_state_sync(s);
    StateSync g; REQUIRE(decode_state_sync(bytes, g));
    CHECK(g.frame == 7u);
    CHECK(g.blob == std::vector<uint8_t>{1, 2, 3, 250, 0, 99});
}

TEST_CASE("net: checksum round-trips") {
    Checksum c; c.frame = 900; c.crc = 0x12345678;
    auto bytes = encode_checksum(c);
    Checksum g; REQUIRE(decode_checksum(bytes, g));
    CHECK(g.frame == 900u); CHECK(g.crc == 0x12345678u);
}

TEST_CASE("net: decoders reject short buffers") {
    std::vector<uint8_t> empty, tiny{ (uint8_t)MsgType::Input, 1, 2 };
    MsgType t; CHECK_FALSE(peek_type(empty, t));
    Input g; CHECK_FALSE(decode_input(tiny, g));
    Hello h; CHECK_FALSE(decode_hello(tiny, h));
}
