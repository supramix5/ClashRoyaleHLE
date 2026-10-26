// Audio (platform/AudioOutput.h voices; XAudio2 on Windows, an SDL software mixer on Linux):
//  * OpenAL (sound effects): buffers point at the app's PCM (alBufferDataStatic); each source is a voice.
//  * AudioFile + AudioQueue (music, AAC .m4a): files are decoded once to 16-bit PCM (platform/MediaDecode.h). The app's
//    own queue logic runs unchanged: AudioFileReadPackets hands out packet ranges (1024 frames each), each enqueued
//    buffer plays that range of PCM, and when a buffer finishes the app's queue callback runs on the main run loop to
//    refill it (looping and stopping are the app's decisions, as on iOS).
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "libc/Files.h"
#include "Log.h"
#include "Memory.h"
#include "platform/AudioOutput.h"
#include "platform/MediaDecode.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using hle::arg;
using hle::ret;

namespace {
std::recursive_mutex g_lock;

// ======================================== OpenAL ========================================
struct ALBuffer { std::uint64_t data = 0; std::uint32_t size = 0; int channels = 1, bits = 16, rate = 44100; };
struct ALSource {
    std::unique_ptr<platform::Voice> voice;
    int channels = 0, rate = 0, bits = 0; // the voice's format
    std::uint32_t buffer = 0;
    bool looping = false;
    float gain = 1, pitch = 1;
};
std::map<std::uint32_t, ALBuffer> g_buffers;
std::map<std::uint32_t, ALSource> g_sources;
std::uint32_t g_nextBuffer = 1, g_nextSource = 1;
constexpr int AL_BUFFER = 0x1009, AL_LOOPING = 0x1007, AL_GAIN = 0x100A, AL_PITCH = 0x1003, AL_SOURCE_STATE = 0x1010;
constexpr int AL_INITIAL = 0x1011, AL_PLAYING = 0x1012, AL_STOPPED = 0x1014;
// AL_FORMAT_MONO8 0x1100, MONO16 0x1101, STEREO8 0x1102, STEREO16 0x1103.
void stopSource(ALSource &s) {
    if (!s.voice) return;
    s.voice->stop();
    s.voice->flush();
}
void playSource(ALSource &s) {
    auto found = g_buffers.find(s.buffer);
    if (found == g_buffers.end() || !found->second.data || !platform::audioAvailable()) return;
    const auto &b = found->second;
    if (s.voice && (s.channels != b.channels || s.rate != b.rate || s.bits != b.bits)) s.voice.reset();
    if (!s.voice) {
        s.voice = platform::createVoice(b.channels, b.rate, b.bits);
        if (!s.voice) return;
        s.channels = b.channels;
        s.rate = b.rate;
        s.bits = b.bits;
    }
    stopSource(s);
    s.voice->enqueue(mem::base() + b.data, b.size, 0, s.looping ? -1 : 0);
    s.voice->setVolume(s.gain);
    s.voice->setPitch(s.pitch);
    s.voice->start();
}
// alBufferDataStatic(buffer, format, data, size, frequency): the data stays owned by the app.
void alBufferDataStatic() {
    std::lock_guard lock(g_lock);
    auto &b = g_buffers[std::uint32_t(arg(0))];
    const auto format = int(arg(1));
    b.data = arg(2);
    b.size = std::uint32_t(arg(3));
    b.rate = int(arg(4));
    b.channels = format == 0x1102 || format == 0x1103 ? 2 : 1;
    b.bits = format == 0x1100 || format == 0x1102 ? 8 : 16;
}
} // namespace

HLE(alcOpenDevice) { platform::audioAvailable(); ret(mem::hleAlloc(16)); }
HLE(alcCloseDevice) { ret(1); }
HLE(alcCreateContext) { ret(mem::hleAlloc(16)); }
HLE(alcDestroyContext) {}
HLE(alcMakeContextCurrent) { ret(1); }
HLE(alcProcessContext) {}
HLE(alcSuspendContext) {}
HLE(alcGetError) { ret(0); }
HLE(alGetError) { ret(0); }
HLE(alcGetProcAddress) {
    const auto name = mem::cstring(arg(1));
    if (name == "alBufferDataStatic") {
        static std::uint64_t stub = hle::makeStub("alBufferDataStatic", alBufferDataStatic);
        return ret(stub);
    }
    hlog("audio", "alcGetProcAddress(%s): not available", name.c_str());
    ret(0);
}
HLE(alGenBuffers) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) { mem::w32(arg(1) + 4 * i, g_nextBuffer); g_buffers[g_nextBuffer++] = {}; }
}
HLE(alDeleteBuffers) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) g_buffers.erase(mem::r32(arg(1) + 4 * i));
}
HLE(alBufferData) {
    // Copy semantics: keep a private copy in guest memory.
    std::lock_guard lock(g_lock);
    auto &b = g_buffers[std::uint32_t(arg(0))];
    const auto format = int(arg(1));
    b.size = std::uint32_t(arg(3));
    b.data = mem::alloc(b.size);
    std::memcpy(mem::base() + b.data, mem::base() + arg(2), b.size);
    b.rate = int(arg(4));
    b.channels = format == 0x1102 || format == 0x1103 ? 2 : 1;
    b.bits = format == 0x1100 || format == 0x1102 ? 8 : 16;
}
HLE(alGenSources) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) { mem::w32(arg(1) + 4 * i, g_nextSource); g_sources[g_nextSource++] = {}; }
}
HLE(alDeleteSources) {
    std::lock_guard lock(g_lock);
    for (std::uint64_t i = 0; i < arg(0); ++i) {
        auto found = g_sources.find(mem::r32(arg(1) + 4 * i));
        if (found == g_sources.end()) continue;
        g_sources.erase(found); // the voice goes with it
    }
}
HLE(alSourcei) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    if (int(arg(1)) == AL_BUFFER) { stopSource(s); s.buffer = std::uint32_t(arg(2)); }
    else if (int(arg(1)) == AL_LOOPING) s.looping = arg(2) != 0;
}
HLE(alSourcef) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    const float value = cpu::s(0);
    if (int(arg(1)) == AL_GAIN) { s.gain = value; if (s.voice) s.voice->setVolume(value); }
    else if (int(arg(1)) == AL_PITCH) { s.pitch = value; if (s.voice) s.voice->setPitch(value); }
}
HLE(alSourcePlay) { std::lock_guard lock(g_lock); playSource(g_sources[std::uint32_t(arg(0))]); }
HLE(alSourceStop) { std::lock_guard lock(g_lock); stopSource(g_sources[std::uint32_t(arg(0))]); }
HLE(alSourcePause) { std::lock_guard lock(g_lock); auto &s = g_sources[std::uint32_t(arg(0))]; if (s.voice) s.voice->stop(); }
HLE(alGetSourcei) {
    std::lock_guard lock(g_lock);
    auto &s = g_sources[std::uint32_t(arg(0))];
    int value = 0;
    if (int(arg(1)) == AL_SOURCE_STATE) {
        value = !s.voice ? AL_INITIAL : s.voice->queued() ? AL_PLAYING : AL_STOPPED;
    } else if (int(arg(1)) == AL_BUFFER) value = int(s.buffer);
    else if (int(arg(1)) == AL_LOOPING) value = s.looping;
    mem::w32(arg(2), std::uint32_t(value));
}
HLE(alListener3f) {}
HLE(alListenerf) {}
HLE(alSource3f) {}
HLE(alDistanceModel) {}

// ======================================== AudioFile / AudioQueue (music) ========================================
namespace {
struct Track {
    std::vector<std::int16_t> pcm; // interleaved stereo, 44.1 kHz
    std::uint64_t frames = 0, packets = 0;
    std::uint64_t bytes = 0;
};
std::map<std::uint64_t, std::shared_ptr<Track>> g_files; // AudioFileID -> decoded track
std::map<std::string, std::shared_ptr<Track>> g_decoded;   // host path -> decoded track
std::uint64_t g_nextFile = 0x9000;
constexpr int kFramesPerPacket = 1024;

std::shared_ptr<Track> decode(const std::string &hostPath) {
    if (auto found = g_decoded.find(hostPath); found != g_decoded.end()) return found->second;
    auto track = std::make_shared<Track>();
    platform::decodeMediaFile(hostPath, track->pcm);
    track->frames = track->pcm.size() / 2;
    track->packets = (track->frames + kFramesPerPacket - 1) / kFramesPerPacket;
    std::error_code ec;
    track->bytes = std::uint64_t(std::filesystem::file_size(std::filesystem::u8path(hostPath), ec));
    hlog("audio", "decoded %s: %.1f s", std::filesystem::u8path(hostPath).filename().string().c_str(), track->frames / 44100.0);
    g_decoded[hostPath] = track;
    return track;
}

// A queue: a voice; buffers carry their packet range in their first 24 bytes (written by AudioFileReadPackets):
// {magic, file id, start packet, packet count}. Each enqueued buffer's token is the AudioQueueBuffer address with the
// queue's generation in the top 16 bits (AudioQueueStop bumps it: buffers from before are not reported).
constexpr std::uint64_t kRangeMagic = 0x484C4541514E4752ull; // "RGNQAELH"
struct Queue {
    std::uint64_t handle = 0, callback = 0, userData = 0;
    std::unique_ptr<platform::Voice> voice;
    float volume = 1;
    bool started = false, disposed = false;
    std::uint64_t generation = 0;
};
std::map<std::uint64_t, std::unique_ptr<Queue>> g_queues;
// A buffer finished (audio thread): back to the app on the main run loop, callback(userData, queue, buffer).
void bufferEnded(std::uint64_t handle, std::uint64_t token) {
    const auto buffer = token & 0xFFFFFFFFFFFFull;
    const auto generation = token >> 48;
    runloop::post([handle, buffer, generation] {
        std::uint64_t callback, userData;
        {
            std::lock_guard lock(g_lock);
            auto found = g_queues.find(handle);
            if (found == g_queues.end() || found->second->disposed || (found->second->generation & 0xFFFF) != generation) return;
            callback = found->second->callback;
            userData = found->second->userData;
        }
        // HLE_AUDIOLOG=1: each AudioQueue refill (tests that music keeps streaming past its first buffers).
        static const bool log = std::getenv("HLE_AUDIOLOG") != nullptr;
        if (log) hlog("audio", "queue %llx refills buffer %llx", (unsigned long long)handle, (unsigned long long)buffer);
        cpu::call(callback, {userData, handle, buffer});
    });
}
Queue *queue(std::uint64_t handle) {
    auto found = g_queues.find(handle);
    return found == g_queues.end() ? nullptr : found->second.get();
}
} // namespace

HLE(AudioFileOpenURL) {
    const auto path = ns::text(objc::send(arg(0), "path"));
    const auto host = files::hostPath(path);
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::u8path(host), ec)) { mem::w64(arg(3), 0); return ret(std::uint64_t(std::int64_t(-43))); }
    std::lock_guard lock(g_lock);
    const auto track = decode(host);
    const auto handle = g_nextFile++;
    g_files[handle] = track;
    mem::w64(arg(3), handle);
    ret(0);
}
HLE(AudioFileClose) { std::lock_guard lock(g_lock); g_files.erase(arg(0)); ret(0); }
// AudioFileGetProperty(file, id, ioSize *, data): 'dfmt' format, 'bcnt' byte count, 'pcnt' packet count,
// 'pkub' packet upper bound, 'psze' maximum packet size.
HLE(AudioFileGetProperty) {
    std::lock_guard lock(g_lock);
    auto found = g_files.find(arg(0));
    if (found == g_files.end()) return ret(std::uint64_t(std::int64_t(-50)));
    const auto &t = *found->second;
    switch (std::uint32_t(arg(1))) {
    case 0x64666D74: // 'dfmt': AAC, 44.1 kHz stereo, 1024 frames per packet
        mem::write<double>(arg(3), 44100.0);
        mem::w32(arg(3) + 8, 0x61616320);
        mem::w32(arg(3) + 12, 0);
        mem::w32(arg(3) + 16, 0);
        mem::w32(arg(3) + 20, kFramesPerPacket);
        mem::w32(arg(3) + 24, 0);
        mem::w32(arg(3) + 28, 2);
        mem::w32(arg(3) + 32, 0);
        mem::w32(arg(3) + 36, 0);
        break;
    case 0x62636E74: mem::w64(arg(3), t.bytes); break;   // 'bcnt'
    case 0x70636E74: mem::w64(arg(3), t.packets); break; // 'pcnt'
    case 0x706B7562: case 0x7073697A: case 0x70737A65: mem::w32(arg(3), 1536); break; // 'pkub' / max packet size
    default: return ret(std::uint64_t(std::int64_t(-50)));
    }
    ret(0);
}
HLE(AudioFileGetPropertyInfo) { ret(std::uint64_t(std::int64_t(-50))); } // no magic cookie or channel layout
// AudioFileReadPackets(file, useCache, outNumBytes *, packetDescriptions, startPacket, ioNumPackets *, buffer)
HLE(AudioFileReadPackets) {
    std::lock_guard lock(g_lock);
    auto found = g_files.find(arg(0));
    const auto start = arg(4);
    std::uint64_t count = arg(5) ? mem::r32(arg(5)) : 0;
    if (found == g_files.end() || start >= found->second->packets) count = 0;
    else count = std::min<std::uint64_t>(count, found->second->packets - start);
    if (count && arg(6)) {
        mem::w64(arg(6), kRangeMagic);
        mem::w64(arg(6) + 8, arg(0));
        mem::w64(arg(6) + 16, start);
        mem::w64(arg(6) + 24, count);
    }
    // AudioStreamPacketDescription {i64 start offset, u32 variable frames, u32 size}: 32 bytes for the first
    // packet (the range header), 0 for the rest.
    if (arg(3))
        for (std::uint64_t i = 0; i < count; ++i) {
            mem::w64(arg(3) + 16 * i, i ? 32 : 0);
            mem::w32(arg(3) + 16 * i + 8, 0);
            mem::w32(arg(3) + 16 * i + 12, i ? 0 : 32);
        }
    if (arg(2)) mem::w32(arg(2), count ? 32 : 0);
    if (arg(5)) mem::w32(arg(5), std::uint32_t(count));
    ret(0);
}
// AudioQueueNewOutput(format, callback, userData, runLoop, runLoopMode, flags, outQueue)
HLE(AudioQueueNewOutput) {
    std::lock_guard lock(g_lock);
    auto q = std::make_unique<Queue>();
    q->handle = mem::calloc(64);
    q->callback = arg(1);
    q->userData = arg(2);
    const auto handle = q->handle;
    q->voice = platform::createVoice(2, 44100, 16, [handle](std::uint64_t token) { bufferEnded(handle, token); });
    mem::w64(arg(6), q->handle);
    g_queues[q->handle] = std::move(q);
    ret(0);
}
// AudioQueueBuffer: capacity +0, data +8, byteSize +16, userData +24, packetDescCapacity +32, packetDescs +40, count +48.
HLE(AudioQueueAllocateBuffer) {
    const auto buffer = mem::calloc(64);
    mem::w32(buffer, std::uint32_t(arg(1)));
    mem::w64(buffer + 8, mem::calloc(std::size_t(std::max<std::uint64_t>(arg(1), 64))));
    mem::w64(arg(2), buffer);
    ret(0);
}
HLE(AudioQueueAllocateBufferWithPacketDescriptions) {
    const auto buffer = mem::calloc(64);
    mem::w32(buffer, std::uint32_t(arg(1)));
    mem::w64(buffer + 8, mem::calloc(std::size_t(std::max<std::uint64_t>(arg(1), 64))));
    mem::w32(buffer + 32, std::uint32_t(arg(2)));
    mem::w64(buffer + 40, mem::calloc(std::size_t(arg(2)) * 16 + 16));
    mem::w64(arg(3), buffer);
    ret(0);
}
// AudioQueueEnqueueBuffer(queue, buffer, numPacketDescs, packetDescs): plays the buffer's packet range.
HLE(AudioQueueEnqueueBuffer) {
    std::lock_guard lock(g_lock);
    auto *q = queue(arg(0));
    const auto buffer = arg(1), data = mem::r64(buffer + 8);
    if (!q || !q->voice || mem::r64(data) != kRangeMagic) return ret(0);
    auto file = g_files.find(mem::r64(data + 8));
    if (file == g_files.end()) return ret(0);
    const auto &t = *file->second;
    const auto first = mem::r64(data + 16) * kFramesPerPacket;
    const auto last = std::min<std::uint64_t>(t.frames, (mem::r64(data + 16) + mem::r64(data + 24)) * kFramesPerPacket);
    if (first >= last) return ret(0);
    q->voice->enqueue(t.pcm.data() + first * 2, std::size_t((last - first) * 4), buffer | ((q->generation & 0xFFFF) << 48));
    ret(0);
}
HLE(AudioQueueStart) {
    std::lock_guard lock(g_lock);
    if (auto *q = queue(arg(0)); q && q->voice) { q->voice->setVolume(q->volume); q->voice->start(); q->started = true; }
    ret(0);
}
HLE(AudioQueuePause) {
    std::lock_guard lock(g_lock);
    if (auto *q = queue(arg(0)); q && q->voice) q->voice->stop();
    ret(0);
}
HLE(AudioQueueStop) {
    std::lock_guard lock(g_lock);
    if (auto *q = queue(arg(0)); q && q->voice) {
        q->voice->stop();
        ++q->generation;
        q->voice->flush();
        q->started = false;
    }
    ret(0);
}
HLE(AudioQueueDispose) {
    std::lock_guard lock(g_lock);
    if (auto *q = queue(arg(0))) {
        q->disposed = true;
        if (q->voice) { q->voice->stop(); q->voice->flush(); q->voice.reset(); }
    }
    ret(0);
}
// AudioQueueSetParameter(queue, kAudioQueueParam_Volume = 1, value in s0)
HLE(AudioQueueSetParameter) {
    std::lock_guard lock(g_lock);
    if (auto *q = queue(arg(0)); q && arg(1) == 1) { q->volume = cpu::s(0); if (q->voice) q->voice->setVolume(q->volume); }
    ret(0);
}
HLE(AudioQueueFreeBuffer) { ret(0); }
HLE(AudioQueueSetProperty) { ret(0); }
HLE(AudioQueuePrime) { ret(0); }
