#include "engine.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ---------------- BufferSource ----------------
BufferSource::BufferSource(AudioBufferPtr buf, int64_t begin, int64_t end, float gain)
    : buf_(std::move(buf)), begin_(begin), end_(end), pos_(begin), gain_(gain) {
    if (buf_) {
        int64_t nf = buf_->frames();
        begin_ = std::max<int64_t>(0, std::min(begin_, nf));
        end_   = std::max(begin_, std::min(end_, nf));
        pos_   = begin_;
    }
}
void BufferSource::seek(int64_t frame) {
    pos_ = begin_ + std::max<int64_t>(0, frame);
    if (pos_ > end_) pos_ = end_;
}
int BufferSource::render(float* out, int frames) {
    if (!buf_) return 0;
    const int ch = buf_->channels;
    const float* s = buf_->samples.data();
    int produced = 0;
    for (int i = 0; i < frames; ++i) {
        if (pos_ >= end_) break;
        const float* fr = s + pos_ * ch;
        float l = fr[0];
        float r = ch > 1 ? fr[1] : fr[0];
        out[i * 2]     = l * gain_;
        out[i * 2 + 1] = r * gain_;
        ++pos_;
        ++produced;
    }
    return produced;
}

// ---------------- TimelineSource ----------------
TimelineSource::TimelineSource(std::vector<TimelineSegment> segs, int64_t totalFrames)
    : segs_(std::move(segs)), total_(totalFrames) {}

int TimelineSource::render(float* out, int frames) {
    if (pos_ >= total_) return 0;
    int n = (int)std::min<int64_t>(frames, total_ - pos_);
    std::memset(out, 0, sizeof(float) * 2 * n);
    for (const auto& seg : segs_) {
        int64_t segEnd = seg.timelineStart + seg.length;
        int64_t a = std::max(pos_, seg.timelineStart);
        int64_t b = std::min<int64_t>(pos_ + n, segEnd);
        if (a >= b) continue;
        const int ch = seg.buf->channels;
        const float* s = seg.buf->samples.data();
        for (int64_t p = a; p < b; ++p) {
            int64_t src = p - seg.timelineStart;
            const float* fr = s + src * ch;
            int oi = (int)(p - pos_);
            out[oi * 2]     += fr[0] * seg.gain;
            out[oi * 2 + 1] += (ch > 1 ? fr[1] : fr[0]) * seg.gain;
        }
    }
    // soft clamp against overlap clipping
    for (int i = 0; i < n * 2; ++i)
        out[i] = std::max(-1.0f, std::min(1.0f, out[i]));
    pos_ += n;
    return n;
}

// ---------------- PlaybackEngine ----------------
struct PlaybackEngine::Impl {
    ComPtr<IMMDeviceEnumerator> enumr;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* mix = nullptr;    // CoTaskMem
    HANDLE hEvent = nullptr;
    UINT32 bufferFrames = 0;

    std::thread thread;
    std::atomic<bool> running{ false };

    std::mutex mtx;
    std::shared_ptr<IPlaybackSource> source;
    bool paused = true;
    bool drained = false;

    std::vector<float> mixbuf;      // stereo scratch

    // format info
    bool isFloat = false;
    int bits = 32;
    int dchannels = 2;

    // resampler (source/project rate -> device rate), active only when they differ
    bool rsActive = false;
    double rsRatio = 1.0;           // source frames advanced per output frame
    double rsFrac = 0.0;
    bool rsPrimed = false;
    float rsPrev[2]{ 0, 0 }, rsNext[2]{ 0, 0 };

    void resetRs() { rsPrimed = false; rsFrac = 0.0; }

    // Pull one stereo source frame; false when the source is drained.
    bool pullSrcFrame(float o[2]) {
        float tmp[2];
        if (!source || source->render(tmp, 1) < 1) return false;
        o[0] = tmp[0]; o[1] = tmp[1];
        return true;
    }
    // Linear-resample the source into `frames` device frames. Returns frames
    // produced; fewer than requested means the source drained.
    int renderResampled(float* out, int frames) {
        if (!rsPrimed) {
            if (!pullSrcFrame(rsPrev)) return 0;
            if (!pullSrcFrame(rsNext)) { rsNext[0] = rsPrev[0]; rsNext[1] = rsPrev[1]; }
            rsFrac = 0.0; rsPrimed = true;
        }
        int produced = 0;
        for (int i = 0; i < frames; ++i) {
            out[i * 2]     = rsPrev[0] + (float)rsFrac * (rsNext[0] - rsPrev[0]);
            out[i * 2 + 1] = rsPrev[1] + (float)rsFrac * (rsNext[1] - rsPrev[1]);
            ++produced;
            rsFrac += rsRatio;
            bool ended = false;
            while (rsFrac >= 1.0) {
                rsFrac -= 1.0;
                rsPrev[0] = rsNext[0]; rsPrev[1] = rsNext[1];
                if (!pullSrcFrame(rsNext)) { ended = true; break; }
            }
            if (ended) return produced;
        }
        return produced;
    }
};

PlaybackEngine::PlaybackEngine() = default;
PlaybackEngine::~PlaybackEngine() { shutdown(); }

bool PlaybackEngine::init(std::wstring* err) {
    d_ = std::make_unique<Impl>();
    auto fail = [&](const wchar_t* m) { if (err) *err = m; shutdown(); return false; };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)d_->enumr.GetAddressOf());
    if (FAILED(hr)) return fail(L"No audio device enumerator.");

    hr = d_->enumr->GetDefaultAudioEndpoint(eRender, eConsole, &d_->device);
    if (FAILED(hr)) return fail(L"No default audio output device.");

    hr = d_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        (void**)d_->client.GetAddressOf());
    if (FAILED(hr)) return fail(L"Could not activate audio client.");

    hr = d_->client->GetMixFormat(&d_->mix);
    if (FAILED(hr)) return fail(L"Could not get device mix format.");

    WAVEFORMATEX* wf = d_->mix;
    d_->dchannels = wf->nChannels;
    d_->bits = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        d_->isFloat = true;
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
        d_->isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else {
        d_->isFloat = false; // assume PCM int
    }
    sampleRate_ = (int)wf->nSamplesPerSec;

    REFERENCE_TIME dur = 30 * 10000; // 30 ms
    hr = d_->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, wf, nullptr);
    if (FAILED(hr)) return fail(L"Could not initialize audio client.");

    d_->hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d_->client->SetEventHandle(d_->hEvent);

    hr = d_->client->GetBufferSize(&d_->bufferFrames);
    if (FAILED(hr)) return fail(L"Could not get buffer size.");

    hr = d_->client->GetService(__uuidof(IAudioRenderClient),
        (void**)d_->render.GetAddressOf());
    if (FAILED(hr)) return fail(L"Could not get render client.");

    d_->mixbuf.resize((size_t)d_->bufferFrames * 2);
    // apply any source rate requested before init()
    d_->rsActive = (srcRate_ > 0 && srcRate_ != sampleRate_);
    d_->rsRatio = (srcRate_ > 0) ? (double)srcRate_ / (double)sampleRate_ : 1.0;
    d_->running = true;
    d_->client->Start();
    d_->thread = std::thread([this] { threadMain(); });
    return true;
}

void PlaybackEngine::setSourceRate(int rate) {
    srcRate_ = rate;
    if (!d_) return;
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->rsActive = (rate > 0 && rate != sampleRate_);
    d_->rsRatio = (rate > 0) ? (double)rate / (double)sampleRate_ : 1.0;
    d_->resetRs();
}

void PlaybackEngine::shutdown() {
    if (!d_) return;
    d_->running = false;
    if (d_->hEvent) SetEvent(d_->hEvent);
    if (d_->thread.joinable()) d_->thread.join();
    if (d_->client) d_->client->Stop();
    if (d_->hEvent) CloseHandle(d_->hEvent);
    if (d_->mix) CoTaskMemFree(d_->mix);
    d_.reset();
}

void PlaybackEngine::writeFrames(void* dst, const float* stereo, int frames) {
    const int dch = d_->dchannels;
    if (d_->isFloat) {
        float* o = (float*)dst;
        for (int i = 0; i < frames; ++i) {
            float l = stereo[i * 2], r = stereo[i * 2 + 1];
            for (int c = 0; c < dch; ++c)
                o[i * dch + c] = (dch == 1) ? 0.5f * (l + r) : (c == 0 ? l : c == 1 ? r : 0.0f);
        }
    } else if (d_->bits == 16) {
        int16_t* o = (int16_t*)dst;
        for (int i = 0; i < frames; ++i) {
            float l = stereo[i * 2], r = stereo[i * 2 + 1];
            for (int c = 0; c < dch; ++c) {
                float v = (dch == 1) ? 0.5f * (l + r) : (c == 0 ? l : c == 1 ? r : 0.0f);
                v = std::max(-1.0f, std::min(1.0f, v));
                o[i * dch + c] = (int16_t)lrintf(v * 32767.0f);
            }
        }
    } else { // 32-bit int
        int32_t* o = (int32_t*)dst;
        for (int i = 0; i < frames; ++i) {
            float l = stereo[i * 2], r = stereo[i * 2 + 1];
            for (int c = 0; c < dch; ++c) {
                float v = (dch == 1) ? 0.5f * (l + r) : (c == 0 ? l : c == 1 ? r : 0.0f);
                v = std::max(-1.0f, std::min(1.0f, v));
                o[i * dch + c] = (int32_t)llrint((double)v * 2147483647.0);
            }
        }
    }
}

void PlaybackEngine::threadMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    while (d_->running) {
        DWORD w = WaitForSingleObject(d_->hEvent, 200);
        if (!d_->running) break;
        if (w != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(d_->client->GetCurrentPadding(&padding))) continue;
        UINT32 avail = d_->bufferFrames - padding;
        if (avail == 0) continue;

        BYTE* pData = nullptr;
        if (FAILED(d_->render->GetBuffer(avail, &pData))) continue;

        bool endReached = false;
        int produced = 0;
        {
            std::lock_guard<std::mutex> lk(d_->mtx);
            if (d_->source && !d_->paused && !d_->drained) {
                produced = d_->rsActive ? d_->renderResampled(d_->mixbuf.data(), (int)avail)
                                        : d_->source->render(d_->mixbuf.data(), (int)avail);
                if (produced < (int)avail) {
                    // zero the tail
                    std::memset(d_->mixbuf.data() + produced * 2, 0,
                                sizeof(float) * 2 * (avail - produced));
                    d_->drained = true;
                    endReached = true;
                }
            } else {
                std::memset(d_->mixbuf.data(), 0, sizeof(float) * 2 * avail);
            }
            writeFrames(pData, d_->mixbuf.data(), (int)avail);
        }
        d_->render->ReleaseBuffer(avail, 0);

        if (endReached && endCb_) endCb_();
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
    CoUninitialize();
}

void PlaybackEngine::play(std::shared_ptr<IPlaybackSource> src, int64_t startFrame) {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->source = std::move(src);
    if (d_->source) d_->source->seek(startFrame);
    d_->paused = false;
    d_->drained = false;
    d_->resetRs();
}
void PlaybackEngine::pause() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->paused = true;
}
void PlaybackEngine::resume() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->source && !d_->drained) d_->paused = false;
}
void PlaybackEngine::stop() {
    std::lock_guard<std::mutex> lk(d_->mtx);
    d_->source.reset();
    d_->paused = true;
    d_->drained = false;
    d_->resetRs();
}
void PlaybackEngine::seek(int64_t frame) {
    std::lock_guard<std::mutex> lk(d_->mtx);
    if (d_->source) {
        d_->source->seek(frame);
        d_->drained = false;
        d_->resetRs();
    }
}
bool PlaybackEngine::isPlaying() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->source && !d_->paused && !d_->drained;
}
bool PlaybackEngine::isPaused() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->source && d_->paused;
}
bool PlaybackEngine::hasSource() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return (bool)d_->source;
}
int64_t PlaybackEngine::position() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->source ? d_->source->position() : 0;
}
int64_t PlaybackEngine::total() const {
    std::lock_guard<std::mutex> lk(d_->mtx);
    return d_->source ? d_->source->total() : 0;
}
