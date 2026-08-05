#include "render/SurfaceRing.h"

namespace rp {

SurfaceRing::SurfaceRing(uint32_t slot_count)
    : slot_count_(slot_count == 0 ? 1 : slot_count) {}

uint64_t SurfaceRing::reallocate(uint32_t width, uint32_t height) {
    width_ = width; height_ = height;
    ++generation_;
    producer_cursor_ = 0;
    has_ready_.store(false, std::memory_order_relaxed);
    return generation_;
}

uint32_t SurfaceRing::next_producer_slot() {
    uint32_t s = producer_cursor_;
    producer_cursor_ = (producer_cursor_ + 1) % slot_count_;
    return s;
}

bool SurfaceRing::accept_submit(uint32_t index, uint64_t generation) {
    if (generation != generation_) return false;
    if (index >= slot_count_) return false;
    ready_index_.store(index, std::memory_order_relaxed);
    ready_generation_.store(generation, std::memory_order_relaxed);
    has_ready_.store(true, std::memory_order_release);
    return true;
}

bool SurfaceRing::latest_ready(uint32_t& index_out) const {
    if (!has_ready_.load(std::memory_order_acquire)) return false;
    if (ready_generation_.load(std::memory_order_relaxed) != generation_) return false;
    index_out = ready_index_.load(std::memory_order_relaxed);
    return true;
}
}
