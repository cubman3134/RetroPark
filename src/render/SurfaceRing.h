#pragma once
#include <atomic>
#include <cstdint>

namespace rp {
class SurfaceRing {
public:
    explicit SurfaceRing(uint32_t slot_count);
    uint64_t reallocate(uint32_t width, uint32_t height);
    uint32_t next_producer_slot();
    bool     accept_submit(uint32_t index, uint64_t generation, uint64_t sync_value);
    bool     latest_ready(uint32_t& index_out, uint64_t& sync_value_out) const;

    uint64_t generation() const { return generation_; }
    uint32_t slot_count() const { return slot_count_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    uint32_t slot_count_;
    uint32_t width_ = 0, height_ = 0;
    uint64_t generation_ = 0;
    uint32_t producer_cursor_ = 0;
    // Written by accept_submit (core's render thread) and read by latest_ready
    // (host/present thread); synchronized via acquire/release on has_ready_.
    std::atomic<bool>     has_ready_{false};
    std::atomic<uint32_t> ready_index_{0};
    std::atomic<uint64_t> ready_generation_{0};
    std::atomic<uint64_t> ready_sync_{0};
};
}
