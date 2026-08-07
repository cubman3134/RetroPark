#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include "audio/IAudioOutput.h"
#include <vector>
#include <mutex>

namespace rp {
class XAudio2Output : public IAudioOutput {
public:
    ~XAudio2Output() override;
    rp_result open(uint32_t sample_rate, uint32_t channels, std::string& err) override;
    void submit(const int16_t* frames, size_t num_frames) override;
    void close() override;
    void on_buffer_end(void* ctx);   // called by the voice callback

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
};
}
