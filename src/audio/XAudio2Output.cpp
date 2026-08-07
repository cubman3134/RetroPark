#include "audio/XAudio2Output.h"
#include "audio/AudioPool.h"

namespace rp {

struct VoiceCB : public IXAudio2VoiceCallback {
    XAudio2Output* owner;
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) override { owner->on_buffer_end(ctx); }
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

XAudio2Output::~XAudio2Output() { close(); }

rp_result XAudio2Output::open(uint32_t sample_rate, uint32_t channels, std::string& err) {
    channels_ = channels;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Only S_OK means WE newly initialized COM on this thread; S_FALSE (already
    // initialized) and RPC_E_CHANGED_MODE (initialized elsewhere, different
    // concurrency model) both mean someone else owns it -- don't uninit.
    com_inited_ = (hr == S_OK);
    if (FAILED(XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR))) { err="XAudio2Create"; return RP_ERR_DEVICE; }
    if (FAILED(engine_->CreateMasteringVoice(&master_))) { err="CreateMasteringVoice"; return RP_ERR_DEVICE; }
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = sample_rate; wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;
    cb_ = new VoiceCB(); cb_->owner = this;
    if (FAILED(engine_->CreateSourceVoice(&source_, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, cb_))) {
        err="CreateSourceVoice"; return RP_ERR_DEVICE;
    }
    bufs_.assign(kPoolSize, {});
    source_->Start(0);
    return RP_OK;
}

void XAudio2Output::submit(const int16_t* frames, size_t num_frames) {
    if (!source_ || !frames || num_frames == 0) return;
    int slot;
    { std::lock_guard<std::mutex> lk(mtx_); slot = audio_pick_free_slot(in_use_, kPoolSize);
      if (slot < 0) return;                 // pool full -> drop
      in_use_[slot] = true; }
    size_t samples = num_frames * channels_;
    bufs_[slot].assign(frames, frames + samples);
    XAUDIO2_BUFFER b{};
    b.AudioBytes = (UINT32)(bufs_[slot].size() * sizeof(int16_t));
    b.pAudioData = reinterpret_cast<const BYTE*>(bufs_[slot].data());
    b.pContext = reinterpret_cast<void*>((uintptr_t)slot);
    if (FAILED(source_->SubmitSourceBuffer(&b))) {
        std::lock_guard<std::mutex> lk(mtx_); in_use_[slot] = false;
    }
}

void XAudio2Output::on_buffer_end(void* ctx) {
    uint32_t slot = (uint32_t)(uintptr_t)ctx;
    std::lock_guard<std::mutex> lk(mtx_);
    if (slot < kPoolSize) in_use_[slot] = false;
}

void XAudio2Output::close() {
    if (source_) { source_->Stop(0); source_->FlushSourceBuffers(); source_->DestroyVoice(); source_=nullptr; }
    if (master_) { master_->DestroyVoice(); master_=nullptr; }
    if (engine_) { engine_->Release(); engine_=nullptr; }
    delete cb_; cb_=nullptr;
    if (com_inited_) { CoUninitialize(); com_inited_=false; }
}
}
