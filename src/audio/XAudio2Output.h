#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#include "audio/IAudioOutput.h"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace rp {
class XAudio2Output : public IAudioOutput {
public:
    ~XAudio2Output() override;
    rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) override;
    void submit(const int16_t* frames, size_t num_frames) override;
    void close() override;
    void on_buffer_end(void* ctx);   // called by the voice callback

    // Flow control: frames needed to top the source-voice backlog back up to target_frames_ (0 if full).
    // A puller-based producer calls this each poll and pulls exactly this many, so its average rate locks
    // to the voice's real consumption clock -- the fix for free-running-timer under/overrun crackle.
    size_t want_frames() override;
    uint32_t min_queued_frames() const override { return min_queued_.load(std::memory_order_relaxed); }
    uint32_t starvation_events() const override { return starvations_.load(std::memory_order_relaxed); }

private:
    static const uint32_t kPoolSize = 16;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    struct VoiceCB* cb_ = nullptr;
    uint32_t channels_ = 2;
    bool com_inited_ = false;
    std::mutex mtx_;
    std::vector<std::vector<int16_t>> bufs_;   // kPoolSize
    bool in_use_[kPoolSize] = {false};
    // Adaptive-feeder state. queued_frames_ = frames submitted to the voice but not yet finished playing
    // (bumped in submit, drained in on_buffer_end -- both can run on different threads, hence atomic).
    // target_frames_ = the backlog want_frames() steers toward (~40 ms). min_queued_/starvations_ are
    // read-only health telemetry for the gated audio test.
    uint32_t target_frames_ = 1920;                 // 40 ms @ 48 kHz; recomputed in open()
    std::atomic<uint32_t> queued_frames_{0};
    std::atomic<uint32_t> min_queued_{0xFFFFFFFFu};
    std::atomic<uint32_t> starvations_{0};
    std::atomic<bool>     started_{false};           // set once the first buffer is submitted
};
}
