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

    // Flow control for a puller-based producer (presenting cores): how many stereo frames the output can
    // accept right now to reach its target queue depth (0 = full). A producer pulls exactly this many so
    // its average rate locks to the true playback clock. Default returns a fixed chunk so a backend that
    // does not queue (or a null-device path) keeps producing at a steady nominal rate.
    virtual size_t want_frames() { return 480; }
    // Queue-health telemetry (test/diagnostics): the smallest queued-frame depth ever observed by
    // want_frames() after playback began, and the number of want_frames() calls that saw an empty queue
    // (a genuine underrun/starvation event). Defaults are the "always healthy" values.
    virtual uint32_t min_queued_frames() const { return 0xFFFFFFFFu; }
    virtual uint32_t starvation_events() const { return 0; }
};
}
