// platform/AudioOutput.h on Windows: one XAudio2 source voice per Voice. Each enqueued buffer carries its token as the
// XAudio2 buffer context; OnBufferEnd reports it (also for buffers dropped by flush, as XAudio2 does).
#include "platform/AudioOutput.h"
#include "Log.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <mutex>
#pragma comment(lib, "xaudio2.lib")

namespace platform {
namespace {
IXAudio2 *g_xaudio = nullptr;
IXAudio2MasteringVoice *g_master = nullptr;
std::mutex g_engineLock;
bool g_tried = false;

class XAudioVoice : public Voice, IXAudio2VoiceCallback {
public:
    XAudioVoice(std::function<void(std::uint64_t)> onBufferEnd) : onBufferEnd_(std::move(onBufferEnd)) {}
    bool create(int channels, int rate, int bits) {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = WORD(channels);
        f.nSamplesPerSec = DWORD(rate);
        f.wBitsPerSample = WORD(bits);
        f.nBlockAlign = WORD(channels * bits / 8);
        f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
        return SUCCEEDED(g_xaudio->CreateSourceVoice(&voice_, &f, 0, 4.0f, this));
    }
    ~XAudioVoice() override {
        if (voice_) voice_->DestroyVoice(); // waits for callbacks in progress
    }
    void enqueue(const void *pcm, std::size_t bytes, std::uint64_t token, int loops) override {
        XAUDIO2_BUFFER x{};
        x.AudioBytes = UINT32(bytes);
        x.pAudioData = static_cast<const BYTE *>(pcm);
        x.pContext = reinterpret_cast<void *>(token);
        if (loops) {
            x.Flags = XAUDIO2_END_OF_STREAM;
            x.LoopCount = loops < 0 ? XAUDIO2_LOOP_INFINITE : UINT32(loops);
        }
        voice_->SubmitSourceBuffer(&x);
    }
    void start() override { voice_->Start(); }
    void stop() override { voice_->Stop(); }
    void flush() override { voice_->FlushSourceBuffers(); }
    void setVolume(float volume) override { voice_->SetVolume(volume); }
    void setPitch(float ratio) override { voice_->SetFrequencyRatio(std::clamp(ratio, 0.05f, 4.0f)); }
    std::size_t queued() override {
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }
    std::uint64_t framesPlayed() override {
        XAUDIO2_VOICE_STATE state{};
        voice_->GetState(&state);
        return state.SamplesPlayed;
    }

private:
    void STDMETHODCALLTYPE OnBufferEnd(void *context) override {
        if (onBufferEnd_) onBufferEnd_(reinterpret_cast<std::uint64_t>(context));
    }
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}

    IXAudio2SourceVoice *voice_ = nullptr;
    std::function<void(std::uint64_t)> onBufferEnd_;
};
} // namespace

bool audioAvailable() {
    std::lock_guard lock(g_engineLock);
    if (g_xaudio) return true;
    if (g_tried) return false;
    g_tried = true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(g_xaudio->CreateMasteringVoice(&g_master))) {
        hlog("audio", "XAudio2 unavailable; audio stays silent");
        g_xaudio = nullptr;
        return false;
    }
    hlog("audio", "XAudio2 output ready");
    return true;
}

std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void(std::uint64_t)> onBufferEnd) {
    if (!audioAvailable()) return nullptr;
    auto v = std::make_unique<XAudioVoice>(std::move(onBufferEnd));
    if (!v->create(channels, rate, bits)) return nullptr;
    return v;
}
} // namespace platform
