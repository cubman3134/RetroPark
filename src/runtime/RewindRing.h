#pragma once
#include <deque>
#include <vector>
#include <cstdint>

namespace rp {

// Push a snapshot onto a bounded rewind ring, dropping the oldest entries until the ring
// holds at most `max` snapshots. Front = oldest, back = newest; order is preserved. Pure and
// device-free so the ring bookkeeping can be unit-tested in isolation (see test_savestate).
inline void rewind_ring_push(std::deque<std::vector<uint8_t>>& ring,
                             std::vector<uint8_t> snap, uint32_t max) {
    ring.push_back(std::move(snap));
    while (ring.size() > max) ring.pop_front();
}

} // namespace rp
