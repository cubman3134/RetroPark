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
    close();   // guard against double-open: tear down any prior live instance first (idempotent/null-safe)
    channels_ = channels;
    // Adaptive feeder target: keep ~40 ms of audio queued on the source voice. Deep enough to ride out
    // producer-poll jitter (~15 ms Windows sleep granularity) without starving, shallow enough that added
    // latency stays imperceptible. Reset the depth counter + health telemetry for this session.
    target_frames_ = sample_rate ? sample_rate * 40u / 1000u : 1920u;
    queued_frames_.store(0, std::memory_order_relaxed);
    min_queued_.store(0xFFFFFFFFu, std::memory_order_relaxed);
    starvations_.store(0, std::memory_order_relaxed);
    started_.store(false, std::memory_order_relaxed);
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
    // Grow the backlog BEFORE submitting. Once the buffer is live on the voice its OnBufferEnd can fire
    // immediately on the callback thread, and that callback's saturating subtract must find this add
    // already applied. If we added AFTER submit, a preempt in between would let the subtract clamp the
    // debt to 0 and then this add would leak +num_frames permanently -- queued_frames_ would ratchet up
    // until want_frames() reports "full" forever and the feeder starves. Roll back (saturating) on failure.
    queued_frames_.fetch_add((uint32_t)num_frames, std::memory_order_relaxed);
    if (FAILED(source_->SubmitSourceBuffer(&b))) {
        { std::lock_guard<std::mutex> lk(mtx_); in_use_[slot] = false; }
        uint32_t q = queued_frames_.load(std::memory_order_relaxed);
        while (!queued_frames_.compare_exchange_weak(
                   q, q > num_frames ? q - (uint32_t)num_frames : 0u, std::memory_order_relaxed)) {}
        return;
    }
    started_.store(true, std::memory_order_relaxed);
}

void XAudio2Output::on_buffer_end(void* ctx) {
    uint32_t slot = (uint32_t)(uintptr_t)ctx;
    uint32_t done_frames = 0;
    { std::lock_guard<std::mutex> lk(mtx_);
      if (slot < kPoolSize) {
          done_frames = channels_ ? (uint32_t)(bufs_[slot].size() / channels_) : 0;
          in_use_[slot] = false;
      } }
    // This buffer finished playing: shrink the backlog. Saturating subtract so a stray double-callback
    // can never underflow the unsigned depth into a huge value that would stall want_frames().
    if (done_frames) {
        uint32_t q = queued_frames_.load(std::memory_order_relaxed);
        while (!queued_frames_.compare_exchange_weak(
                   q, q > done_frames ? q - done_frames : 0u, std::memory_order_relaxed)) {}
    }
}

size_t XAudio2Output::want_frames() {
    uint32_t q = queued_frames_.load(std::memory_order_relaxed);
    if (started_.load(std::memory_order_relaxed)) {
        // Telemetry: track the shallowest backlog seen, and count empty-queue (starvation) events -- a
        // healthy feeder keeps this at zero. (Only meaningful once the first buffer has been submitted.)
        uint32_t prev = min_queued_.load(std::memory_order_relaxed);
        while (q < prev && !min_queued_.compare_exchange_weak(prev, q, std::memory_order_relaxed)) {}
        if (q == 0) starvations_.fetch_add(1, std::memory_order_relaxed);
    }
    if (q >= target_frames_) return 0;            // full -> pull nothing
    return (size_t)(target_frames_ - q);          // deficit (<= target_frames_, inherently bounded)
}

void XAudio2Output::close() {
    if (source_) { source_->Stop(0); source_->FlushSourceBuffers(); source_->DestroyVoice(); source_=nullptr; }
    if (master_) { master_->DestroyVoice(); master_=nullptr; }
    if (engine_) { engine_->Release(); engine_=nullptr; }
    delete cb_; cb_=nullptr;
    if (com_inited_) { CoUninitialize(); com_inited_=false; }
}
}
