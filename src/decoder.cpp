#include "decoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace mfio {

bool startup() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

void shutdown() {
    MFShutdown();
    CoUninitialize();
}

AudioBufferPtr decodeFile(const std::wstring& path, int targetRate, std::wstring* err) {
    auto fail = [&](const wchar_t* m) -> AudioBufferPtr {
        if (err) *err = m;
        return nullptr;
    };

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) return fail(L"Could not open file (unsupported or missing).");

    // Deselect all, enable only the first audio stream.
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // Ask the reader to convert to canonical: float PCM, 2ch, targetRate.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)targetRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * 4);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, targetRate * 2 * 4);
    outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType.Get());
    if (FAILED(hr)) return fail(L"No audio stream or format not supported.");

    // Read back the actual negotiated type (channels/rate should match our request).
    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual);
    UINT32 chans = 2, rate = (UINT32)targetRate;
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &chans);
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (chans == 0) chans = 2;
    if (rate == 0) rate = (UINT32)targetRate;

    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = (int)rate;
    out->channels = (int)chans;

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) return fail(L"Error while decoding audio.");
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            // Format changed mid-stream (rare) - just continue with what we have.
        }
        if (!sample) continue;

        ComPtr<IMFMediaBuffer> mbuf;
        if (FAILED(sample->ConvertToContiguousBuffer(&mbuf))) continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(mbuf->Lock(&data, &maxLen, &curLen))) continue;
        const float* f = reinterpret_cast<const float*>(data);
        size_t n = curLen / sizeof(float);
        out->samples.insert(out->samples.end(), f, f + n);
        mbuf->Unlock();
    }

    if (out->samples.empty()) return fail(L"File contained no decodable audio.");
    return out;
}

} // namespace mfio
