// platform/AudioOutput.h on Linux: voices mixed in software into one SDL audio stream (stereo, 16-bit, 44.1 kHz). Each
// voice plays its queue of buffers in order, stepping through the PCM at rate * pitch / 44100 frames per output frame
// (linear interpolation); a finished buffer's token is reported after the mix (flush drops buffers without reports).
#include "platform/AudioOutput.h"
#include "Log.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace platform {
namespace {
constexpr int kRate = 44100;
class MixerVoice;
SDL_AudioDeviceID g_device = 0;
std::vector<MixerVoice *> g_voices; // guarded by the SDL audio device lock
void mix(void *, Uint8 *stream, int length);

bool engine() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            hlog("audio", "SDL audio unavailable (%s); audio stays silent", SDL_GetError());
            return;
        }
        SDL_AudioSpec want{}, have{};
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = mix;
        g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!g_device) {
            hlog("audio", "no audio device (%s); audio stays silent", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(g_device, 0);
        hlog("audio", "SDL audio output ready (%s)", SDL_GetCurrentAudioDriver());
    });
    return g_device != 0;
}
struct Lock {
    Lock() { SDL_LockAudioDevice(g_device); }
    ~Lock() { SDL_UnlockAudioDevice(g_device); }
};

class MixerVoice final : public Voice {
public:
    MixerVoice(int channels, int rate, int bits, std::function<void(std::uint64_t)> onBufferEnd)
        : channels_(channels), rate_(rate), bits_(bits), onBufferEnd_(std::move(onBufferEnd)) {
        Lock lock;
        g_voices.push_back(this);
    }
    ~MixerVoice() override {
        Lock lock;
        g_voices.erase(std::find(g_voices.begin(), g_voices.end(), this));
    }
    void enqueue(const void *pcm, std::size_t bytes, std::uint64_t token, int loops) override {
        const auto frames = bytes / std::size_t(channels_ * bits_ / 8);
        if (!frames) return;
        Lock lock;
        queue_.push_back({static_cast<const std::uint8_t *>(pcm), frames, token, loops});
    }
    void start() override { Lock lock; playing_ = true; }
    void stop() override { Lock lock; playing_ = false; }
    void flush() override {
        Lock lock;
        queue_.clear();
        position_ = 0;
    }
    void setVolume(float volume) override { Lock lock; volume_ = volume; }
    void setPitch(float ratio) override { Lock lock; pitch_ = std::clamp(ratio, 0.05f, 4.0f); }
    std::size_t queued() override { Lock lock; return queue_.size(); }
    std::uint64_t framesPlayed() override { Lock lock; return played_; }

    // Adds this voice to `out` (stereo float); finished buffers' tokens go to `ended`.
    void render(float *out, int frames, std::vector<std::pair<MixerVoice *, std::uint64_t>> &ended) {
        if (!playing_) return;
        const double step = double(rate_) * pitch_ / kRate;
        for (int i = 0; i < frames && !queue_.empty(); ++i) {
            auto *b = &queue_.front();
            auto index = std::size_t(position_);
            while (index >= b->frames) {
                position_ -= double(b->frames);
                if (b->loops != 0) {
                    if (b->loops > 0) --b->loops;
                } else {
                    ended.emplace_back(this, b->token);
                    queue_.pop_front();
                    if (queue_.empty()) { position_ = 0; return; }
                    b = &queue_.front();
                }
                index = std::size_t(position_);
            }
            const double t = position_ - double(index);
            const auto next = index + 1 < b->frames ? index + 1 : index;
            for (int c = 0; c < 2; ++c) {
                const int source = channels_ == 1 ? 0 : c;
                const float a = sample(*b, index, source), v = sample(*b, next, source);
                out[2 * i + c] += float(a + (v - a) * t) * volume_;
            }
            position_ += step;
            const auto whole = std::size_t(position_);
            if (whole > index) played_ += whole - index;
        }
    }
    void bufferEnded(std::uint64_t token) {
        if (onBufferEnd_) onBufferEnd_(token);
    }

private:
    struct Buffer {
        const std::uint8_t *data;
        std::size_t frames;
        std::uint64_t token;
        int loops;
    };
    float sample(const Buffer &b, std::size_t frame, int channel) const {
        const std::size_t at = frame * std::size_t(channels_) + std::size_t(channel);
        if (bits_ == 8) return (float(b.data[at]) - 128.f) / 128.f;
        std::int16_t v;
        std::memcpy(&v, b.data + 2 * at, 2);
        return float(v) / 32768.f;
    }
    int channels_, rate_, bits_;
    std::function<void(std::uint64_t)> onBufferEnd_;
    std::deque<Buffer> queue_;
    double position_ = 0; // in the front buffer
    std::uint64_t played_ = 0;
    float volume_ = 1, pitch_ = 1;
    bool playing_ = false;
};

// The SDL audio callback (the device lock is held): mix every voice, then report finished buffers. Callbacks only post
// to the run loop (AudioOutput.h), so running them here, under the lock, is safe.
void mix(void *, Uint8 *stream, int length) {
    const int frames = length / 4;
    static std::vector<float> accumulator;
    accumulator.assign(std::size_t(frames) * 2, 0.f);
    std::vector<std::pair<MixerVoice *, std::uint64_t>> ended;
    for (auto *voice : g_voices) voice->render(accumulator.data(), frames, ended);
    auto *out = reinterpret_cast<std::int16_t *>(stream);
    for (std::size_t i = 0; i < accumulator.size(); ++i)
        out[i] = std::int16_t(std::lround(std::clamp(accumulator[i], -1.f, 1.f) * 32767.f));
    for (auto &[voice, token] : ended) voice->bufferEnded(token);
}
} // namespace

bool audioAvailable() { return engine(); }
std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void(std::uint64_t)> onBufferEnd) {
    if (!engine()) return nullptr;
    return std::make_unique<MixerVoice>(channels, rate, bits, std::move(onBufferEnd));
}
} // namespace platform
