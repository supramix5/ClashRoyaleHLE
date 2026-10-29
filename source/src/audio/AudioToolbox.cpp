// AudioToolbox ExtAudioFile for WAV sound effects, decoding PCM, Microsoft IMA ADPCM, and Microsoft ADPCM.
// AudioStreamBasicDescription (40 bytes): mSampleRate (double) +0, mFormatID +8, mFormatFlags +12, mBytesPerPacket +16,
// mFramesPerPacket +20, mBytesPerFrame +24, mChannelsPerFrame +28, mBitsPerChannel +32.
#include "foundation/Foundation.h"
#include "Log.h"
#include "Memory.h"
#include "libc/Files.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

using hle::arg;
using hle::ret;
namespace {
struct AudioFile {
    std::vector<std::int16_t> samples; // interleaved 16-bit
    std::vector<std::int16_t> clientSamples;
    int channels = 1, rate = 44100;
    int clientChannels = 1, clientRate = 44100;
    std::uint64_t frames = 0, clientFrames = 0, position = 0;
    int clientBits = 16;
};
std::mutex g_lock;
std::map<std::uint64_t, AudioFile> g_files;
std::uint64_t g_next = 0x7000;
std::uint32_t u32(const std::string &b, std::size_t o) { std::uint32_t v; std::memcpy(&v, b.data() + o, 4); return v; }
std::uint16_t u16(const std::string &b, std::size_t o) { std::uint16_t v; std::memcpy(&v, b.data() + o, 2); return v; }
// Microsoft IMA ADPCM (WAVE format 0x11): per block, a 4-byte header per channel {i16 predictor, u8 index, u8 0},
// then 4-byte groups per channel of 8 nibbles each (low nibble first).
const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStepTable[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
                            107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
                            796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
                            4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
                            20350, 22385, 24623, 27086, 29794, 32767};
std::int16_t imaStep(int nibble, int &predictor, int &index) {
    const int step = kStepTable[index];
    int diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    predictor = std::clamp(predictor + diff, -32768, 32767);
    index = std::clamp(index + kIndexTable[nibble & 15], 0, 88);
    return std::int16_t(predictor);
}
void decodeIma(const unsigned char *data, std::size_t size, int channels, int blockAlign, std::vector<std::int16_t> &out) {
    for (std::size_t block = 0; block + std::size_t(4 * channels) <= size; block += std::size_t(blockAlign)) {
        const auto *b = data + block;
        const std::size_t blockBytes = std::min<std::size_t>(std::size_t(blockAlign), size - block);
        int predictor[2] = {}, index[2] = {};
        for (int c = 0; c < channels; ++c) {
            predictor[c] = std::int16_t(b[4 * c] | (b[4 * c + 1] << 8));
            index[c] = std::clamp(int(b[4 * c + 2]), 0, 88);
            out.push_back(std::int16_t(predictor[c]));
        }
        if (channels == 2 && out.size() >= 2) {} // header samples already interleaved L, R
        std::size_t offset = std::size_t(4 * channels);
        while (offset + std::size_t(4 * channels) <= blockBytes) {
            std::int16_t samples[2][8];
            for (int c = 0; c < channels; ++c)
                for (int k = 0; k < 4; ++k) {
                    const unsigned char byte = b[offset + std::size_t(4 * c + k)];
                    samples[c][2 * k] = imaStep(byte & 15, predictor[c], index[c]);
                    samples[c][2 * k + 1] = imaStep(byte >> 4, predictor[c], index[c]);
                }
            for (int i = 0; i < 8; ++i)
                for (int c = 0; c < channels; ++c) out.push_back(samples[c][i]);
            offset += std::size_t(4 * channels);
        }
    }
}
// Microsoft ADPCM (WAVE format 2): per block and channel a predictor index, an initial delta and two samples
// (output second sample first), then nibbles high first, alternating channels for stereo.
void decodeMsAdpcm(const unsigned char *data, std::size_t size, int channels, int blockAlign, std::vector<std::int16_t> &out) {
    static const int kAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    static const int kCoef1[7] = {256, 512, 0, 192, 240, 460, 392}, kCoef2[7] = {0, -256, 0, 64, 0, -208, -232};
    const std::size_t header = std::size_t(7 * channels);
    for (std::size_t block = 0; block + header <= size; block += std::size_t(blockAlign)) {
        const auto *b = data + block;
        const std::size_t blockBytes = std::min<std::size_t>(std::size_t(blockAlign), size - block);
        int predictor[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < channels; ++c) {
            predictor[c] = std::min(int(b[c]), 6);
            delta[c] = std::int16_t(b[channels + 2 * c] | (b[channels + 2 * c + 1] << 8));
            s1[c] = std::int16_t(b[3 * channels + 2 * c] | (b[3 * channels + 2 * c + 1] << 8));
            s2[c] = std::int16_t(b[5 * channels + 2 * c] | (b[5 * channels + 2 * c + 1] << 8));
        }
        for (int c = 0; c < channels; ++c) out.push_back(std::int16_t(s2[c]));
        for (int c = 0; c < channels; ++c) out.push_back(std::int16_t(s1[c]));
        int c = 0;
        auto nibble = [&](int n) {
            const int signedNibble = n & 8 ? n - 16 : n;
            int sample = ((s1[c] * kCoef1[predictor[c]]) + (s2[c] * kCoef2[predictor[c]])) / 256 + signedNibble * delta[c];
            sample = std::clamp(sample, -32768, 32767);
            out.push_back(std::int16_t(sample));
            s2[c] = s1[c];
            s1[c] = sample;
            delta[c] = std::max(16, (kAdapt[n] * delta[c]) / 256);
            c = (c + 1) % channels;
        };
        for (std::size_t o = header; o < blockBytes; ++o) {
            nibble(b[o] >> 4);
            nibble(b[o] & 15);
        }
    }
}
bool parseWav(const std::string &bytes, AudioFile &f) {
    if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") || bytes.compare(8, 4, "WAVE")) return false;
    int bits = 16, format = 1, blockAlign = 0;
    std::uint32_t factFrames = 0;
    bool hasFact = false;
    for (std::size_t o = 12; o + 8 <= bytes.size();) {
        const auto id = bytes.substr(o, 4);
        const auto size = u32(bytes, o + 4);
        if (id == "fmt ") {
            format = u16(bytes, o + 8);
            f.channels = u16(bytes, o + 10);
            f.rate = int(u32(bytes, o + 12));
            blockAlign = u16(bytes, o + 20);
            bits = u16(bytes, o + 22);
        } else if (id == "fact") {
            if (size < 4) return false;
            factFrames = u32(bytes, o + 8);
            hasFact = true;
        } else if (id == "data") {
            const auto end = std::min<std::size_t>(bytes.size(), o + 8 + size);
            if (format == 0x11) {
                if (f.channels < 1 || f.channels > 2 || !blockAlign) return false;
                decodeIma(reinterpret_cast<const unsigned char *>(bytes.data()) + o + 8, end - o - 8, f.channels, blockAlign, f.samples);
            } else if (format == 2) {
                if (f.channels < 1 || f.channels > 2 || !blockAlign) return false;
                decodeMsAdpcm(reinterpret_cast<const unsigned char *>(bytes.data()) + o + 8, end - o - 8, f.channels, blockAlign, f.samples);
            } else if (format != 1) {
                return false;
            } else if (bits == 16) {
                f.samples.resize((end - o - 8) / 2);
                std::memcpy(f.samples.data(), bytes.data() + o + 8, f.samples.size() * 2);
            } else if (bits == 24 || bits == 32) {
                const std::size_t step = std::size_t(bits / 8); // keep the top 16 bits of each little-endian sample
                for (std::size_t i = o + 8; i + step <= end; i += step)
                    f.samples.push_back(std::int16_t(static_cast<unsigned char>(bytes[i + step - 2]) | (static_cast<unsigned char>(bytes[i + step - 1]) << 8)));
            } else if (bits == 8) {
                for (std::size_t i = o + 8; i < end; ++i) f.samples.push_back(std::int16_t((static_cast<unsigned char>(bytes[i]) - 128) << 8));
            } else return false;
        }
        o += 8 + size + (size & 1);
    }
    if (f.channels < 1 || f.channels > 2 || f.rate <= 0) return false;
    if (hasFact && (format == 0x11 || format == 2)) {
        const auto decodedFrames = f.samples.size() / std::size_t(f.channels);
        if (factFrames > decodedFrames) return false;
        f.samples.resize(std::size_t(factFrames) * std::size_t(f.channels));
    }
    f.frames = f.channels ? f.samples.size() / std::size_t(f.channels) : 0;
    f.clientChannels = f.channels;
    f.clientRate = f.rate;
    f.clientFrames = f.frames;
    return true;
}
void convertClientFormat(AudioFile &f) {
    f.clientSamples.clear();
    f.clientFrames = f.frames;
    if (f.clientRate != f.rate)
        f.clientFrames = std::uint64_t(std::llround(double(f.frames) * f.clientRate / f.rate));
    if (f.clientChannels == f.channels && f.clientRate == f.rate) return;

    f.clientSamples.resize(std::size_t(f.clientFrames) * std::size_t(f.clientChannels));
    for (std::uint64_t frame = 0; frame < f.clientFrames; ++frame) {
        const double sourcePosition = double(frame) * f.rate / f.clientRate;
        const auto first = std::min<std::uint64_t>(std::uint64_t(sourcePosition), f.frames - 1);
        const auto second = std::min(first + 1, f.frames - 1);
        const double fraction = sourcePosition - double(first);
        double source[2]{};
        for (int channel = 0; channel < f.channels; ++channel) {
            const auto firstSample = f.samples[std::size_t(first) * std::size_t(f.channels) + std::size_t(channel)];
            const auto secondSample = f.samples[std::size_t(second) * std::size_t(f.channels) + std::size_t(channel)];
            source[channel] = firstSample + (secondSample - firstSample) * fraction;
        }
        for (int channel = 0; channel < f.clientChannels; ++channel) {
            const double sample = f.channels == 1 ? source[0] :
                (f.clientChannels == 1 ? (source[0] + source[1]) * 0.5 : source[channel]);
            f.clientSamples[std::size_t(frame) * std::size_t(f.clientChannels) + std::size_t(channel)] =
                std::int16_t(std::clamp(std::lround(sample), -32768l, 32767l));
        }
    }
}
void writeAsbd(std::uint64_t p, const AudioFile &f, int bits, bool clientFormat) {
    const int rate = clientFormat ? f.clientRate : f.rate;
    const int channels = clientFormat ? f.clientChannels : f.channels;
    mem::write<double>(p, double(rate));
    mem::w32(p + 8, 0x6C70636D); // 'lpcm'
    mem::w32(p + 12, 0xC);       // signed integer, packed
    mem::w32(p + 16, std::uint32_t(channels * bits / 8));
    mem::w32(p + 20, 1);
    mem::w32(p + 24, std::uint32_t(channels * bits / 8));
    mem::w32(p + 28, std::uint32_t(channels));
    mem::w32(p + 32, std::uint32_t(bits));
    mem::w32(p + 36, 0);
}
} // namespace

// ExtAudioFileOpenURL(CFURLRef, ExtAudioFileRef *)
HLE(ExtAudioFileOpenURL) {
    const auto path = ns::text(objc::send(arg(0), "path"));
    std::string bytes;
    AudioFile f;
    if (!files::readWholeFile(path, bytes) || !parseWav(bytes, f)) {
        hlog("audio", "ExtAudioFileOpenURL %s: unsupported or missing", path.c_str());
        return ret(std::uint64_t(std::int64_t(-43)));
    }
    std::lock_guard lock(g_lock);
    const auto handle = g_next++;
    g_files[handle] = std::move(f);
    mem::w64(arg(1), handle);
    ret(0);
}
// ExtAudioFileGetProperty(file, id, ioSize *, data): 'ffmt' file format, 'cfmt' client format, '#frm' frame count.
HLE(ExtAudioFileGetProperty) {
    std::lock_guard lock(g_lock);
    auto found = g_files.find(arg(0));
    if (found == g_files.end()) return ret(std::uint64_t(std::int64_t(-50)));
    const auto &f = found->second;
    switch (std::uint32_t(arg(1))) {
    case 0x66666D74: writeAsbd(arg(3), f, 16, false); break;           // 'ffmt'
    case 0x63666D74: writeAsbd(arg(3), f, f.clientBits, true); break; // 'cfmt'
    case 0x2366726D: mem::w64(arg(3), f.frames); break;         // '#frm'
    default: return ret(std::uint64_t(std::int64_t(-50)));
    }
    ret(0);
}
HLE(ExtAudioFileSetProperty) {
    std::lock_guard lock(g_lock);
    auto found = g_files.find(arg(0));
    if (found == g_files.end()) return ret(std::uint64_t(std::int64_t(-50)));
    if (std::uint32_t(arg(1)) == 0x63666D74) {
        auto &f = found->second;
        const double rate = mem::read<double>(arg(3));
        const auto channels = int(mem::r32(arg(3) + 28));
        const auto bits = int(mem::r32(arg(3) + 32));
        const auto roundedRate = std::isfinite(rate) && rate >= 0.5 && rate <= 384000.5 ? std::lround(rate) : 0;
        if (!std::isfinite(rate) || roundedRate < 1 || roundedRate > 384000 || channels < 1 || channels > 2 ||
            (bits != 8 && bits != 16) || (f.frames && !f.rate)) {
            hlog("audio", "ExtAudioFileSetProperty: unsupported client format (%.0f Hz, %d channels, %d bits)",
                 rate, channels, bits);
            return ret(std::uint64_t(std::int64_t(-50)));
        }
        f.clientRate = int(roundedRate);
        f.clientChannels = channels;
        f.clientBits = bits;
        convertClientFormat(f);
        f.position = 0;
    }
    ret(0);
}
// ExtAudioFileRead(file, UInt32 *ioNumberFrames, AudioBufferList *): buffers at +8 {channels, byteSize, data}.
HLE(ExtAudioFileRead) {
    std::lock_guard lock(g_lock);
    auto found = g_files.find(arg(0));
    if (found == g_files.end()) return ret(std::uint64_t(std::int64_t(-50)));
    auto &f = found->second;
    const auto list = arg(2);
    const int bytesPerFrame = f.clientChannels * f.clientBits / 8;
    const auto capacity = mem::r32(list + 12) / std::uint32_t(std::max(1, bytesPerFrame));
    const auto wanted = std::min<std::uint64_t>(std::min<std::uint64_t>(mem::r32(arg(1)), capacity), f.clientFrames - f.position);
    const auto out = mem::r64(list + 16);
    const auto &samples = f.clientSamples.empty() ? f.samples : f.clientSamples;
    for (std::uint64_t i = 0; i < wanted * std::uint64_t(f.clientChannels); ++i) {
        const auto sample = samples[std::size_t(f.position * std::uint64_t(f.clientChannels) + i)];
        if (f.clientBits == 8) mem::write<std::uint8_t>(out + i, std::uint8_t((sample >> 8) + 128));
        else mem::write<std::int16_t>(out + 2 * i, sample);
    }
    f.position += wanted;
    mem::w32(arg(1), std::uint32_t(wanted));
    mem::w32(list + 12, std::uint32_t(wanted * std::uint64_t(bytesPerFrame)));
    ret(0);
}
HLE(ExtAudioFileDispose) {
    std::lock_guard lock(g_lock);
    g_files.erase(arg(0));
    ret(0);
}
HLE(AudioServicesPlaySystemSound) {}
