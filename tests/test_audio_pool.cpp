#include <doctest/doctest.h>
#include "audio/AudioPool.h"
using rp::audio_pick_free_slot;

TEST_CASE("audio pool: picks first free slot") {
    bool s[4] = {true, true, false, true};
    CHECK(audio_pick_free_slot(s, 4) == 2);
}
TEST_CASE("audio pool: all in use -> -1 (drop-on-full)") {
    bool s[3] = {true, true, true};
    CHECK(audio_pick_free_slot(s, 3) == -1);
}
TEST_CASE("audio pool: first slot free") {
    bool s[3] = {false, true, true};
    CHECK(audio_pick_free_slot(s, 3) == 0);
}
