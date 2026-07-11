#include "encoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <memory>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace mfio {

const wchar_t* extensionFor(ExportFormat f) {
    switch (f) {
    case ExportFormat::WAV: return L".wav";
    case ExportFormat::MP3: return L".mp3";
    case ExportFormat::AAC: return L".m4a";
    case ExportFormat::WMA: return L".wma";
    }
    return L".wav";
}
const wchar_t* labelFor(ExportFormat f) {
    switch (f) {
    case ExportFormat::WAV: return L"WAV (PCM)";
    case ExportFormat::MP3: return L"MP3";
    case ExportFormat::AAC: return L"AAC (.m4a)";
    case ExportFormat::WMA: return L"Windows Media Audio";
    }
    return L"WAV";
}

// Linear-resample a canonical float buffer to dstRate (keeps channel count).
// Returns the original data unchanged when the rate already matches.
static AudioBufferPtr resampleTo(const AudioBuffer& buf, int dstRate) {
    auto out = std::make_shared<AudioBuffer>();
    out->channels = buf.channels;
    out->sampleRate = dstRate;
    int ch = buf.channels;
    int64_t nf = buf.frames();
    if (dstRate <= 0 || dstRate == buf.sampleRate || nf == 0) {
        out->sampleRate = (dstRate > 0) ? dstRate : buf.sampleRate;
        out->samples = buf.samples;
        return out;
    }
    double ratio = (double)buf.sampleRate / (double)dstRate;   // src frames / out frame
    int64_t outFrames = (int64_t)((double)nf * dstRate / buf.sampleRate);
    out->samples.resize((size_t)outFrames * ch);
    const float* s = buf.samples.data();
    for (int64_t i = 0; i < outFrames; ++i) {
        double srcPos = (double)i * ratio;
        int64_t i0 = (int64_t)srcPos;
        double frac = srcPos - (double)i0;
        int64_t i1 = std::min<int64_t>(i0 + 1, nf - 1);
        for (int c = 0; c < ch; ++c) {
            float a = s[i0 * ch + c];
            float b = s[i1 * ch + c];
            out->samples[(size_t)i * ch + c] = a + (float)frac * (b - a);
        }
    }
    return out;
}

// Convert canonical stereo float -> interleaved int16 with requested channel count.
static std::vector<int16_t> toInt16(const AudioBuffer& buf, int channels) {
    int64_t nf = buf.frames();
    int sc = buf.channels;
    std::vector<int16_t> out((size_t)nf * channels);
    const float* s = buf.samples.data();
    auto clip = [](float v) { return v < -1.f ? -1.f : v > 1.f ? 1.f : v; };
    for (int64_t f = 0; f < nf; ++f) {
        float l = s[f * sc];
        float r = sc > 1 ? s[f * sc + 1] : l;
        if (channels == 1) {
            out[f] = (int16_t)lrintf(clip(0.5f * (l + r)) * 32767.0f);
        } else {
            out[f * 2]     = (int16_t)lrintf(clip(l) * 32767.0f);
            out[f * 2 + 1] = (int16_t)lrintf(clip(r) * 32767.0f);
        }
    }
    return out;
}

// ---------------- WAV (manual RIFF; 16/24-bit int PCM or 32-bit float) ----------------
static bool writeWav(const std::wstring& path, const AudioBuffer& buf,
                     const ExportOptions& opts, std::wstring* err) {
    int bits = opts.bitsPerSample;
    if (bits != 16 && bits != 24 && bits != 32) bits = 16;
    const bool isFloat = (bits == 32);
    const int ch = opts.channels;
    const int sc = buf.channels;
    const int64_t nf = buf.frames();
    const int bytesPerSample = bits / 8;

    auto clip = [](float v) { return v < -1.f ? -1.f : v > 1.f ? 1.f : v; };

    std::vector<uint8_t> data;
    data.reserve((size_t)nf * ch * bytesPerSample);
    const float* s = buf.samples.data();
    for (int64_t f = 0; f < nf; ++f) {
        float l = s[f * sc];
        float r = sc > 1 ? s[f * sc + 1] : l;
        for (int c = 0; c < ch; ++c) {
            float v = clip((ch == 1) ? 0.5f * (l + r) : (c == 0 ? l : r));
            if (bits == 16) {
                int16_t q = (int16_t)lrintf(v * 32767.0f);
                data.push_back((uint8_t)(q & 0xff));
                data.push_back((uint8_t)((q >> 8) & 0xff));
            } else if (bits == 24) {
                int32_t q = (int32_t)lrintf(v * 8388607.0f);
                data.push_back((uint8_t)(q & 0xff));
                data.push_back((uint8_t)((q >> 8) & 0xff));
                data.push_back((uint8_t)((q >> 16) & 0xff));
            } else { // 32-bit IEEE float
                float fv = v;
                uint8_t b[4]; std::memcpy(b, &fv, 4);
                data.insert(data.end(), b, b + 4);
            }
        }
    }

    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) { if (err) *err = L"Could not open output file."; return false; }

    uint32_t dataBytes = (uint32_t)data.size();
    uint16_t nch = (uint16_t)ch;
    uint32_t rate = (uint32_t)opts.sampleRate;
    uint16_t blockAlign = (uint16_t)(ch * bytesPerSample);
    uint32_t byteRate = rate * blockAlign;
    uint16_t fmtTag = isFloat ? 3 /*WAVE_FORMAT_IEEE_FLOAT*/ : 1 /*WAVE_FORMAT_PCM*/;
    // float files carry a fact chunk (12 bytes) for strict readers
    uint32_t factBytes = isFloat ? 12 : 0;
    uint32_t riffSize = 36 + factBytes + dataBytes;

    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, fp); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, fp); };
    fwrite("RIFF", 1, 4, fp); w32(riffSize); fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp); w32(16); w16(fmtTag); w16(nch);
    w32(rate); w32(byteRate); w16(blockAlign); w16((uint16_t)bits);
    if (isFloat) { fwrite("fact", 1, 4, fp); w32(4); w32((uint32_t)nf); }
    fwrite("data", 1, 4, fp); w32(dataBytes);
    if (!data.empty()) fwrite(data.data(), 1, dataBytes, fp);
    fclose(fp);
    return true;
}

// ---------------- MF encode (MP3 / AAC / WMA) ----------------
static bool buildOutputType(ExportFormat fmt, const ExportOptions& opts,
                            ComPtr<IMFMediaType>& outType) {
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    int avgBytes = opts.bitrateKbps * 1000 / 8;
    switch (fmt) {
    case ExportFormat::MP3:
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, opts.channels);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, opts.sampleRate);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
        outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        break;
    case ExportFormat::AAC:
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, opts.channels);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, opts.sampleRate);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
        outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        break;
    case ExportFormat::WMA:
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_WMAudioV9);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, opts.channels);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, opts.sampleRate);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avgBytes);
        break;
    default: return false;
    }
    return true;
}

static bool writeMF(const std::wstring& path, const AudioBuffer& buf,
                    const ExportOptions& opts, std::wstring* err) {
    auto fail = [&](const wchar_t* m) { if (err) *err = m; return false; };

    std::vector<int16_t> pcm = toInt16(buf, opts.channels);

    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) return fail(L"Could not create output file for this format.");

    ComPtr<IMFMediaType> outType;
    if (!buildOutputType(opts.format, opts, outType)) return fail(L"Unsupported format.");

    DWORD streamIndex = 0;
    hr = writer->AddStream(outType.Get(), &streamIndex);
    if (FAILED(hr)) return fail(L"This format/bitrate is not supported by Windows here.");

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, opts.channels);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, opts.sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, opts.channels * 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, opts.sampleRate * opts.channels * 2);
    hr = writer->SetInputMediaType(streamIndex, inType.Get(), nullptr);
    if (FAILED(hr)) return fail(L"Encoder rejected the input format.");

    hr = writer->BeginWriting();
    if (FAILED(hr)) return fail(L"Could not begin encoding.");

    const int64_t totalFrames = (int64_t)pcm.size() / opts.channels;
    const int chunk = 4096;
    const int64_t hnsPerSec = 10000000LL;
    int64_t frame = 0;
    while (frame < totalFrames) {
        int n = (int)std::min<int64_t>(chunk, totalFrames - frame);
        DWORD bytes = (DWORD)n * opts.channels * sizeof(int16_t);

        ComPtr<IMFMediaBuffer> mbuf;
        MFCreateMemoryBuffer(bytes, &mbuf);
        BYTE* dst = nullptr; DWORD maxLen = 0;
        mbuf->Lock(&dst, &maxLen, nullptr);
        memcpy(dst, pcm.data() + frame * opts.channels, bytes);
        mbuf->Unlock();
        mbuf->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(mbuf.Get());
        sample->SetSampleTime(frame * hnsPerSec / opts.sampleRate);
        sample->SetSampleDuration((int64_t)n * hnsPerSec / opts.sampleRate);

        hr = writer->WriteSample(streamIndex, sample.Get());
        if (FAILED(hr)) return fail(L"Error while encoding.");
        frame += n;
    }

    hr = writer->Finalize();
    if (FAILED(hr)) return fail(L"Could not finalize the output file.");
    return true;
}

bool encodeFile(const std::wstring& path, const AudioBuffer& buf,
                const ExportOptions& opts, std::wstring* err) {
    // Resample to the requested output rate first so the buffer's rate always
    // matches what the header / encoder is told (otherwise pitch/speed shifts).
    AudioBufferPtr rb = resampleTo(buf, opts.sampleRate);
    if (opts.format == ExportFormat::WAV)
        return writeWav(path, *rb, opts, err);
    return writeMF(path, *rb, opts, err);
}

} // namespace mfio
