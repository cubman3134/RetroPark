#pragma once
#include <cstdint>
#include <vector>
#include "retropark/retropark_abi.h"   // rp_input_state
namespace rp::net {

enum class MsgType : uint8_t { Hello = 1, StateSync = 2, Input = 3, Checksum = 4 };

struct Hello {
    uint32_t abi_version = 0;
    char     core_id[64] = {};   // null-padded
    uint64_t content_hash = 0;
    uint32_t input_delay = 0;
    uint64_t start_frame = 0;
};
struct StateSync { uint64_t frame = 0; std::vector<uint8_t> blob; };
struct Input     { uint64_t frame = 0; uint8_t port = 0; rp_input_state state{}; };
struct Checksum  { uint64_t frame = 0; uint32_t crc = 0; };

// Each encoder prepends a 1-byte MsgType tag, then packs fields little-endian.
std::vector<uint8_t> encode_hello(const Hello&);
std::vector<uint8_t> encode_state_sync(const StateSync&);
std::vector<uint8_t> encode_input(const Input&);
std::vector<uint8_t> encode_checksum(const Checksum&);

// Reads msg[0]; false if msg is empty.
bool peek_type(const std::vector<uint8_t>& msg, MsgType& out);

// Decoders return false on malformed / short input (never read out of bounds).
bool decode_hello(const std::vector<uint8_t>&, Hello&);
bool decode_state_sync(const std::vector<uint8_t>&, StateSync&);
bool decode_input(const std::vector<uint8_t>&, Input&);
bool decode_checksum(const std::vector<uint8_t>&, Checksum&);
} // namespace rp::net
