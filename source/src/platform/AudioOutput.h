#pragma once
// Audio output: voices playing a queue of interleaved PCM buffers (8 or 16 bits, mono or stereo, any rate), with volume
// and pitch. Windows plays them through XAudio2 voices, Linux mixes them in software into one SDL audio stream.
// OpenAL sources play one (possibly looping) buffer; AudioQueue (music) enqueues buffers and refills each one from its
// end callback.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace platform {
class Voice {
public:
    virtual ~Voice() = default;
    // Queues `bytes` of PCM (which must stay valid until its end callback), played once plus `loops` more times
    // (-1: forever). Does not start playback.
    virtual void enqueue(const void *pcm, std::size_t bytes, std::uint64_t token, int loops = 0) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;  // pauses, keeping the queue; start() resumes
    // Drops the queue. End callbacks may still arrive for dropped buffers (XAudio2 reports them, the Linux mixer
    // does not): callers that care tag tokens (AudioQueue uses a generation number).
    virtual void flush() = 0;
    virtual void setVolume(float volume) = 0;
    virtual void setPitch(float ratio) = 0; // clamped to 0.05..4
    virtual std::size_t queued() = 0;       // buffers queued (playing or paused)
    virtual std::uint64_t framesPlayed() = 0;
};
// A voice, or nullptr when there is no audio output. onBufferEnd runs on an audio thread when a buffer has finished
// playing: it must only post work to the run loop (never call guest code).
std::unique_ptr<Voice> createVoice(int channels, int rate, int bits, std::function<void(std::uint64_t token)> onBufferEnd = {});
bool audioAvailable(); // starts the output on first use (logs whether it is ready)
} // namespace platform
