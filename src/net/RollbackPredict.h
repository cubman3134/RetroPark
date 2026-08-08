#pragma once
#include "retropark/retropark_abi.h"
#include <map>
#include <cstdint>
#include <cstring>
namespace rp::net {

// rp_input_state is tightly packed (keys[256] then int16 pad_axes[8] then uint16 pad_buttons -> 274
// bytes, 2-byte alignment, no padding holes), so memcmp is a valid equality test here.
inline bool rb_input_equal(const rp_input_state& a, const rp_input_state& b) {
    return std::memcmp(&a, &b, sizeof(rp_input_state)) == 0;
}

// Predict the remote input for an unconfirmed frame: repeat the last confirmed input, else neutral.
inline rp_input_state rb_predict(const std::map<uint64_t, rp_input_state>& remote, uint64_t confirmed) {
    auto it = remote.find(confirmed);
    if (it != remote.end()) return it->second;
    return rp_input_state{};   // neutral (no confirmed input yet)
}

// Earliest frame f in [from, to] (inclusive) where the REAL remote input differs from what was USED
// (fed) at f. Returns UINT64_MAX if none / empty range. A frame with no real input yet is skipped
// (can't be a confirmed misprediction).
uint64_t rb_first_mispredicted(const std::map<uint64_t, rp_input_state>& real,
                               const std::map<uint64_t, rp_input_state>& used,
                               uint64_t from, uint64_t to);

// Erase all entries with key < floor.
template <class V>
inline void rb_prune_below(std::map<uint64_t, V>& m, uint64_t floor) {
    for (auto it = m.begin(); it != m.end() && it->first < floor; ) it = m.erase(it);
}
} // namespace rp::net
