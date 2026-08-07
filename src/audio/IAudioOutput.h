#pragma once
#include <cstdint>
#include <string>
#include <retropark/retropark_abi.h>
namespace rp {
struct IAudioOutput {
    virtual ~IAudioOutput() = default;
    virtual rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) = 0;
    virtual void submit(const int16_t* frames, size_t num_frames) = 0;   // interleaved int16
    virtual void close() = 0;
};
}
