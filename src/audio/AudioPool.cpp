#include "audio/AudioPool.h"
namespace rp {
int audio_pick_free_slot(const bool* in_use, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) if (!in_use[i]) return (int)i;
    return -1;
}
}
