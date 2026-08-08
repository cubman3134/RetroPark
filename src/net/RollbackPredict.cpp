#include "net/RollbackPredict.h"
#include <cstdint>
namespace rp::net {
uint64_t rb_first_mispredicted(const std::map<uint64_t, rp_input_state>& real,
                               const std::map<uint64_t, rp_input_state>& used,
                               uint64_t from, uint64_t to) {
    if (from > to) return UINT64_MAX;
    for (uint64_t f = from; ; ++f) {
        auto rit = real.find(f);
        if (rit != real.end()) {
            auto uit = used.find(f);
            rp_input_state u = (uit != used.end()) ? uit->second : rp_input_state{};
            if (!rb_input_equal(rit->second, u)) return f;
        }
        if (f == to) break;   // unconditional terminal check BEFORE ++f wraps
    }
    return UINT64_MAX;
}
} // namespace rp::net
