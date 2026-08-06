#include <doctest/doctest.h>
#include "render/SurfaceRing.h"

using rp::SurfaceRing;

TEST_CASE("ring: reallocate bumps generation and size") {
    SurfaceRing r(3);
    uint64_t g0 = r.generation();
    uint64_t g1 = r.reallocate(640, 480);
    CHECK(g1 > g0);
    CHECK(r.width() == 640);
    CHECK(r.height() == 480);
    CHECK(r.slot_count() == 3u);
}

TEST_CASE("ring: producer slots are round robin") {
    SurfaceRing r(3);
    r.reallocate(16,16);
    CHECK(r.next_producer_slot() == 0u);
    CHECK(r.next_producer_slot() == 1u);
    CHECK(r.next_producer_slot() == 2u);
    CHECK(r.next_producer_slot() == 0u);
}

TEST_CASE("ring: valid submit becomes latest_ready") {
    SurfaceRing r(3);
    uint64_t g = r.reallocate(16,16);
    uint32_t idx = r.next_producer_slot();
    CHECK(r.accept_submit(idx, g, 0));
    uint32_t out=99; uint64_t sv=0;
    CHECK(r.latest_ready(out, sv));
    CHECK(out == idx);
}

TEST_CASE("ring: stale generation submit is dropped") {
    SurfaceRing r(3);
    uint64_t g_old = r.reallocate(16,16);
    r.reallocate(32,32);              // new generation
    CHECK_FALSE(r.accept_submit(0, g_old, 0));
    uint32_t out=99; uint64_t sv=0;
    CHECK_FALSE(r.latest_ready(out, sv)); // nothing ready at new generation yet
}

TEST_CASE("ring: out-of-range index is rejected") {
    SurfaceRing r(2);
    uint64_t g = r.reallocate(16,16);
    CHECK_FALSE(r.accept_submit(5, g, 0));
}

TEST_CASE("ring: sync_value round-trips with the ready frame") {
    SurfaceRing r(3);
    uint64_t g = r.reallocate(16,16);
    uint32_t idx = r.next_producer_slot();
    CHECK(r.accept_submit(idx, g, /*sync_value=*/42));
    uint32_t out=99; uint64_t sv=0;
    CHECK(r.latest_ready(out, sv));
    CHECK(out == idx);
    CHECK(sv == 42);
}
