#include "dsp.h"
#include <vector>
#include <complex>
#include <algorithm>
#include <cmath>

namespace dsp {

using cf = std::complex<float>;
static const double PI = 3.14159265358979323846;

// ---------------- radix-2 iterative FFT ----------------
static void fft(std::vector<cf>& a, bool inverse) {
    size_t n = a.size();
    if (n < 2) return;
    // bit reversal
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * PI / len * (inverse ? 1 : -1);
        cf wlen((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                cf u = a[i + k];
                cf v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= (float)n;
}

// ---------------- speech loudness ----------------
double speechLoudness(const AudioBuffer& buf) {
    int ch = buf.channels;
    int64_t nf = buf.frames();
    if (nf <= 0) return 0.0;
    int rate = buf.sampleRate > 0 ? buf.sampleRate : 48000;
    int win = std::max(1, rate / 50);   // ~20 ms windows
    const float* s = buf.samples.data();

    std::vector<double> rms;
    rms.reserve((size_t)(nf / win + 1));
    for (int64_t f = 0; f < nf; f += win) {
        int64_t e = std::min<int64_t>(f + win, nf);
        double acc = 0; int64_t cnt = 0;
        for (int64_t i = f; i < e; ++i) {
            double m = 0; for (int c = 0; c < ch; ++c) m += s[i * ch + c];
            m /= ch; acc += m * m; ++cnt;
        }
        rms.push_back(cnt ? std::sqrt(acc / cnt) : 0.0);
    }
    if (rms.empty()) return 0.0;
    double peak = 0; for (double r : rms) peak = std::max(peak, r);
    if (peak <= 0) return 0.0;
    double thresh = std::max(peak * 0.1, 3e-4);  // active = within ~-20 dB of peak
    double acc = 0; int cnt = 0;
    for (double r : rms) if (r >= thresh) { acc += r * r; ++cnt; }
    if (cnt == 0) { for (double r : rms) { acc += r * r; } cnt = (int)rms.size(); }
    return std::sqrt(acc / std::max(1, cnt));
}

// ---------------- noise reduction ----------------
static void strengthParams(NRStrength s, float& overSub, float& floorBeta, float& wienerAlpha) {
    switch (s) {
    case NRStrength::Light:      overSub = 1.5f; floorBeta = 0.08f; wienerAlpha = 1.0f; break;
    case NRStrength::Medium:     overSub = 2.5f; floorBeta = 0.03f; wienerAlpha = 2.0f; break;
    case NRStrength::Aggressive: overSub = 4.0f; floorBeta = 0.005f; wienerAlpha = 3.5f; break;
    }
}

static std::vector<float> denoiseChannel(const std::vector<float>& in, const NROptions& opts) {
    const int N = 1024;
    const int hop = N / 4;
    const size_t len = in.size();
    if (len < (size_t)N) return in;  // too short to process

    // Hann window
    std::vector<float> win(N);
    for (int i = 0; i < N; ++i) win[i] = 0.5f * (1.0f - std::cos(2.0f * (float)PI * i / (N - 1)));

    size_t nFrames = 1 + (len - N) / hop;

    // STFT magnitudes + phases
    std::vector<std::vector<float>> mag(nFrames, std::vector<float>(N));
    std::vector<std::vector<float>> phase(nFrames, std::vector<float>(N));
    std::vector<double> frameEnergy(nFrames, 0);

    std::vector<cf> buf(N);
    for (size_t fr = 0; fr < nFrames; ++fr) {
        size_t s0 = fr * hop;
        double en = 0;
        for (int i = 0; i < N; ++i) {
            float v = in[s0 + i] * win[i];
            buf[i] = cf(v, 0);
            en += (double)v * v;
        }
        frameEnergy[fr] = en;
        fft(buf, false);
        for (int k = 0; k < N; ++k) {
            mag[fr][k] = std::abs(buf[k]);
            phase[fr][k] = std::arg(buf[k]);
        }
    }

    // Estimate noise magnitude from the lowest-energy ~15% of frames.
    std::vector<size_t> order(nFrames);
    for (size_t i = 0; i < nFrames; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return frameEnergy[a] < frameEnergy[b]; });
    size_t noiseCount = std::max<size_t>(1, nFrames * 15 / 100);
    std::vector<float> noiseMag(N, 0.0f);
    for (size_t i = 0; i < noiseCount; ++i) {
        size_t fr = order[i];
        for (int k = 0; k < N; ++k) noiseMag[k] += mag[fr][k];
    }
    for (int k = 0; k < N; ++k) noiseMag[k] /= (float)noiseCount;

    float overSub, floorBeta, wienerAlpha;
    strengthParams(opts.strength, overSub, floorBeta, wienerAlpha);

    // Process + overlap-add reconstruction
    std::vector<float> out(len, 0.0f);
    std::vector<float> norm(len, 0.0f);
    for (size_t fr = 0; fr < nFrames; ++fr) {
        size_t s0 = fr * hop;
        for (int k = 0; k < N; ++k) {
            float m = mag[fr][k];
            float nm = noiseMag[k];
            float newM;
            if (opts.algorithm == NRAlgorithm::SpectralSubtraction) {
                newM = m - overSub * nm;
                float fl = floorBeta * m;
                if (newM < fl) newM = fl;
            } else { // Wiener
                float snr = (m * m) / (nm * nm + 1e-12f);
                float g = snr / (snr + wienerAlpha);
                newM = m * g;
                float fl = floorBeta * m;
                if (newM < fl) newM = fl;
            }
            buf[k] = std::polar(newM, phase[fr][k]);
        }
        fft(buf, true);
        for (int i = 0; i < N; ++i) {
            float w = win[i];
            out[s0 + i] += buf[i].real() * w;
            norm[s0 + i] += w * w;
        }
    }
    for (size_t i = 0; i < len; ++i)
        if (norm[i] > 1e-6f) out[i] /= norm[i];
    return out;
}

AudioBufferPtr denoise(const AudioBuffer& buf, const NROptions& opts) {
    int ch = buf.channels;
    int64_t nf = buf.frames();
    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = buf.sampleRate;
    out->channels = ch;
    out->samples.resize(buf.samples.size());
    if (nf <= 0) return out;

    for (int c = 0; c < ch; ++c) {
        std::vector<float> chan((size_t)nf);
        for (int64_t i = 0; i < nf; ++i) chan[i] = buf.samples[i * ch + c];
        std::vector<float> clean = denoiseChannel(chan, opts);
        for (int64_t i = 0; i < nf; ++i)
            out->samples[i * ch + c] = std::max(-1.0f, std::min(1.0f, clean[i]));
    }
    return out;
}

} // namespace dsp
