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

// ---------------- Audacity-style profile-based noise gating ----------------
// Reimplements Audacity's Noise Reduction (NoiseReductionBase.cpp) in its
// released configuration: WT_HANN_HANN windows, window size 2048, 4 steps per
// window, DM_SECOND_GREATEST classification, attack 0.02 s / release 0.10 s.
// The streaming window-queue of the original is unrolled into whole-signal
// passes, which is mathematically equivalent (the attack pass runs backward in
// time, the release pass forward, exactly like the queue propagation).

static const int kNRWin  = 2048;            // STFT window (Audacity default)
static const int kNRHop  = kNRWin / 4;      // 4 steps per window (Hann/Hann)
static const int kNRSpec = kNRWin / 2 + 1;  // bins 0..Nyquist

static void nrMakeWindow(std::vector<float>& w) {
    // Periodic Hann; used for both analysis and synthesis (WT_HANN_HANN).
    w.resize(kNRWin);
    for (int i = 0; i < kNRWin; ++i)
        w[i] = 0.5f * (1.0f - (float)std::cos(2.0 * PI * i / kNRWin));
}

NoiseProfile computeNoiseProfile(const AudioBuffer& buf) {
    NoiseProfile p;
    p.sampleRate = buf.sampleRate;
    p.seconds = buf.durationSec();
    const int ch = buf.channels;
    const int64_t nf = buf.frames();
    if (ch <= 0 || nf < kNRWin) return p;   // "Selected noise profile is too short."

    std::vector<float> win; nrMakeWindow(win);
    std::vector<double> sums(kNRSpec, 0.0);
    std::vector<cf> fbuf(kNRWin);
    // Only complete windows, no padding (matches Audacity's profile pass);
    // every channel's windows pool into one statistics set.
    const size_t nFr = (size_t)(1 + (nf - kNRWin) / kNRHop);
    for (int c = 0; c < ch; ++c) {
        for (size_t fr = 0; fr < nFr; ++fr) {
            const int64_t s0 = (int64_t)fr * kNRHop;
            for (int i = 0; i < kNRWin; ++i)
                fbuf[i] = cf(buf.samples[(s0 + i) * ch + c] * win[i], 0.0f);
            fft(fbuf, false);
            for (int k = 0; k < kNRSpec; ++k) {
                const double re = fbuf[k].real(), im = fbuf[k].imag();
                sums[k] += re * re + im * im;
            }
            ++p.windows;
        }
    }
    p.means.resize(kNRSpec);
    for (int k = 0; k < kNRSpec; ++k) p.means[k] = (float)(sums[k] / p.windows);
    return p;
}

// Geometric-mean smoothing of the gain curve across frequency: average the
// logs over [k-bins, k+bins] (Audacity's ApplyFreqSmoothing).
static void nrFreqSmooth(std::vector<float>& g, int bins, std::vector<float>& scratch) {
    if (bins <= 0) return;
    for (int k = 0; k < kNRSpec; ++k) g[k] = std::log(g[k]);
    for (int k = 0; k < kNRSpec; ++k) {
        const int j0 = std::max(0, k - bins);
        const int j1 = std::min(kNRSpec - 1, k + bins);
        float acc = 0.0f;
        for (int j = j0; j <= j1; ++j) acc += g[j];
        scratch[k] = acc / (j1 - j0 + 1);
    }
    for (int k = 0; k < kNRSpec; ++k) g[k] = std::exp(scratch[k]);
}

static std::vector<float> denoiseChannelProfile(const std::vector<float>& in, int rate,
                                                const NoiseProfile& prof,
                                                const NRProfileOptions& opts) {
    const size_t len = in.size();
    // Zero-pad both ends: every real sample gets full overlap coverage and the
    // classifier sees zero-power context beyond the edges, like Audacity's
    // leading/trailing padding in the reduction pass.
    const size_t lead = kNRWin - kNRHop;
    const size_t padded = lead + len + kNRWin;
    const size_t nFr = 1 + (padded - kNRWin) / kNRHop;

    std::vector<float> win; nrMakeWindow(win);
    auto sampleAt = [&](size_t padIdx) -> float {
        return (padIdx >= lead && padIdx - lead < len) ? in[padIdx - lead] : 0.0f;
    };

    // Pass A: per-window power spectra.
    std::vector<float> power(nFr * (size_t)kNRSpec);
    std::vector<cf> fbuf(kNRWin);
    for (size_t fr = 0; fr < nFr; ++fr) {
        const size_t s0 = fr * kNRHop;
        for (int i = 0; i < kNRWin; ++i)
            fbuf[i] = cf(sampleAt(s0 + i) * win[i], 0.0f);
        fft(fbuf, false);
        float* pw = &power[fr * kNRSpec];
        for (int k = 0; k < kNRSpec; ++k) {
            const float re = fbuf[k].real(), im = fbuf[k].imag();
            pw[k] = re * re + im * im;
        }
    }

    // Classify each time-frequency cell: noise iff the SECOND GREATEST power
    // among the 5 windows centred on it stays at or below the threshold
    // sensitivity * ln(10) * mean noise power (out-of-range windows count as
    // zero power, matching the padded queue).
    const int nExamine = 1 + kNRWin / kNRHop;   // 5
    const int center = nExamine / 2;            // 2
    const double sensFactor = (double)opts.sensitivity * std::log(10.0);
    std::vector<uint8_t> isNoise(nFr * (size_t)kNRSpec);
    for (size_t fr = 0; fr < nFr; ++fr) {
        uint8_t* cl = &isNoise[fr * kNRSpec];
        for (int k = 0; k < kNRSpec; ++k) {
            float greatest = 0.0f, second = 0.0f;
            for (int d = -center; d <= nExamine - 1 - center; ++d) {
                const int64_t f2 = (int64_t)fr + d;
                const float pw = (f2 >= 0 && f2 < (int64_t)nFr)
                                     ? power[(size_t)f2 * kNRSpec + k] : 0.0f;
                if (pw >= greatest) { second = greatest; greatest = pw; }
                else if (pw >= second) second = pw;
            }
            cl[k] = second <= sensFactor * prof.means[k] ? 1 : 0;
        }
    }

    // Gains: attenuation for noise cells, unity for signal cells, then
    // exponential attack (backward in time) and release (forward) ramps.
    const float atten = (float)std::pow(10.0, -opts.reductionDb / 20.0);
    const int nAttackBlocks  = 1 + (int)(0.02 * rate / kNRHop);
    const int nReleaseBlocks = 1 + (int)(0.10 * rate / kNRHop);
    const float oneAttack  = (float)std::pow(10.0, -opts.reductionDb / (20.0 * nAttackBlocks));
    const float oneRelease = (float)std::pow(10.0, -opts.reductionDb / (20.0 * nReleaseBlocks));

    std::vector<float>& gain = power;   // reuse storage; power no longer needed
    for (size_t i = 0; i < gain.size(); ++i) gain[i] = isNoise[i] ? atten : 1.0f;
    for (size_t fr = nFr - 1; fr-- > 0; ) {          // attack: toward earlier windows
        const float* nx = &gain[(fr + 1) * kNRSpec];
        float* g = &gain[fr * kNRSpec];
        for (int k = 0; k < kNRSpec; ++k) {
            const float m = nx[k] * oneAttack;
            if (g[k] < m) g[k] = m;
        }
    }
    for (size_t fr = 1; fr < nFr; ++fr) {            // release: toward later windows
        const float* pv = &gain[(fr - 1) * kNRSpec];
        float* g = &gain[fr * kNRSpec];
        for (int k = 0; k < kNRSpec; ++k) {
            const float m = pv[k] * oneRelease;
            if (g[k] < m) g[k] = m;
        }
    }

    // Pass B: frequency-smooth the gains, apply to the spectrum (Reduce: *g,
    // Residue: *(g-1), phase-flipped like Audacity), resynthesize.
    std::vector<float> outPad(padded, 0.0f), norm(padded, 0.0f);
    std::vector<float> grow(kNRSpec), scratch(kNRSpec);
    for (size_t fr = 0; fr < nFr; ++fr) {
        const size_t s0 = fr * kNRHop;
        for (int i = 0; i < kNRWin; ++i)
            fbuf[i] = cf(sampleAt(s0 + i) * win[i], 0.0f);
        fft(fbuf, false);
        std::copy(&gain[fr * kNRSpec], &gain[fr * kNRSpec] + kNRSpec, grow.begin());
        nrFreqSmooth(grow, opts.freqSmoothingBands, scratch);
        for (int k = 0; k < kNRSpec; ++k) {
            const float g = opts.residue ? grow[k] - 1.0f : grow[k];
            fbuf[k] *= g;
            if (k > 0 && k < kNRWin / 2) fbuf[kNRWin - k] *= g;  // conjugate mirror
        }
        fft(fbuf, true);
        for (int i = 0; i < kNRWin; ++i) {
            outPad[s0 + i] += fbuf[i].real() * win[i];
            norm[s0 + i] += win[i] * win[i];
        }
    }
    std::vector<float> out(len, 0.0f);
    for (size_t i = 0; i < len; ++i) {
        const float nn = norm[lead + i];
        if (nn > 1e-6f) out[i] = outPad[lead + i] / nn;
    }
    return out;
}

AudioBufferPtr denoiseWithProfile(const AudioBuffer& buf, const NoiseProfile& profile,
                                  const NRProfileOptions& opts) {
    if (!profile.valid() || profile.sampleRate != buf.sampleRate) return nullptr;
    const int ch = buf.channels;
    const int64_t nf = buf.frames();
    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = buf.sampleRate;
    out->channels = ch;
    out->samples.resize(buf.samples.size());
    if (nf <= 0 || ch <= 0) return out;

    for (int c = 0; c < ch; ++c) {
        std::vector<float> chan((size_t)nf);
        for (int64_t i = 0; i < nf; ++i) chan[i] = buf.samples[i * ch + c];
        std::vector<float> clean = denoiseChannelProfile(chan, buf.sampleRate, profile, opts);
        for (int64_t i = 0; i < nf; ++i)
            out->samples[i * ch + c] = std::max(-1.0f, std::min(1.0f, clean[i]));
    }
    return out;
}

AudioBufferPtr denoise(const AudioBuffer& buf, const NROptions& opts) {
    if (opts.algorithm == NRAlgorithm::Profile) {
        if (!opts.noiseProfile) return nullptr;
        return denoiseWithProfile(buf, *opts.noiseProfile, opts.profile);
    }
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

// ---------------- voice isolation (remove non-speech) ----------------
// Frame analysis at 2048/512 (≈43 ms window, ≈11 ms hop at 48 kHz). Three
// features decide whether a frame is *voiced speech*:
//
//   harmonicity  normalised autocorrelation peak over F0 lags (70–400 Hz),
//                de-biased by the analysis window's own autocorrelation and
//                required to be an interior local maximum, so a sub-70 Hz thump
//                cannot fake a peak at the edge of the search range,
//   spectrum     fraction of energy in the speech band (250–4000 Hz) must be
//                non-trivial, and the fraction below 150 Hz must not dominate
//                (that is the signature of a bump / desk knock / footstep),
//   level        frame RMS relative to the clip's loud-frame reference, so room
//                tone and distant rustle drop out.
//
// Voiced frames are then: runs shorter than ~60 ms dropped (transients can ring
// briefly), grown by a pre-roll and the user's hold (this is what preserves
// unvoiced consonants attached to voiced speech), and gaps shorter than ~150 ms
// merged so words aren't chopped mid-utterance.

static const int kVIWin = 2048;
static const int kVIHop = 512;

static std::vector<float> viMonoMix(const AudioBuffer& buf) {
    const int ch = std::max(1, buf.channels);
    const int64_t nf = buf.frames();
    std::vector<float> m((size_t)std::max<int64_t>(0, nf));
    const float inv = 1.0f / (float)ch;
    for (int64_t i = 0; i < nf; ++i) {
        float a = 0.0f;
        for (int c = 0; c < ch; ++c) a += buf.samples[i * ch + c];
        m[(size_t)i] = a * inv;
    }
    return m;
}

// Autocorrelation of a real sequence via FFT (returns lags 0..n-1).
static void viAutocorr(const std::vector<float>& x, std::vector<cf>& scratch,
                       std::vector<double>& out) {
    const size_t n = scratch.size();
    for (size_t i = 0; i < n; ++i) scratch[i] = cf(i < x.size() ? x[i] : 0.0f, 0.0f);
    fft(scratch, false);
    for (size_t i = 0; i < n; ++i) scratch[i] = cf(std::norm(scratch[i]), 0.0f);
    fft(scratch, true);
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = scratch[i].real();
}

std::vector<uint8_t> detectVoiceFrames(const AudioBuffer& buf, const VoiceIsolateOptions& opts,
                                       int* winOut, int* hopOut) {
    if (winOut) *winOut = kVIWin;
    if (hopOut) *hopOut = kVIHop;
    std::vector<uint8_t> mask;
    const int64_t nf = buf.frames();
    if (nf < kVIWin || buf.channels <= 0) return mask;
    const int rate = buf.sampleRate > 0 ? buf.sampleRate : 48000;
    const float s = std::max(0.0f, std::min(1.0f, opts.sensitivity));

    const std::vector<float> mono = viMonoMix(buf);
    const size_t nFrames = (size_t)(1 + (nf - kVIWin) / kVIHop);

    // Periodic Hann + its own autocorrelation (to de-bias the signal ACF).
    std::vector<float> win(kVIWin);
    for (int i = 0; i < kVIWin; ++i)
        win[i] = 0.5f * (1.0f - (float)std::cos(2.0 * PI * i / kVIWin));
    std::vector<cf> scratch(kVIWin * 2);          // zero-padded: linear autocorrelation
    std::vector<double> wac;
    viAutocorr(win, scratch, wac);
    const double wac0 = wac[0] > 0 ? wac[0] : 1.0;

    const int N2 = kVIWin * 2;                             // zero-padded FFT size
    const int lagMin = std::max(2, rate / 400);            // 400 Hz
    const int lagMax = std::min(kVIWin - 2, rate / 70);    // 70 Hz
    const int binLow    = std::max(1, 150 * N2 / rate);
    const int binSpLo   = std::max(1, 250 * N2 / rate);
    const int binSpHi   = std::min(N2 / 2, 4000 * N2 / rate);
    const int binTop    = N2 / 2;

    std::vector<double> rms(nFrames, 0.0), harm(nFrames, 0.0),
                        lowFrac(nFrames, 0.0), spFrac(nFrames, 0.0);

    for (size_t fr = 0; fr < nFrames; ++fr) {
        const size_t s0 = fr * (size_t)kVIHop;
        double raw = 0.0;
        for (int i = 0; i < kVIWin; ++i) {
            const float v = mono[s0 + i];
            raw += (double)v * v;
            scratch[i] = cf(v * win[i], 0.0f);
        }
        for (int i = kVIWin; i < N2; ++i) scratch[i] = cf(0.0f, 0.0f);
        rms[fr] = std::sqrt(raw / kVIWin);

        // One padded FFT yields both the band powers and (via its inverse) the
        // linear autocorrelation used for harmonicity.
        fft(scratch, false);
        double pLow = 0, pSpeech = 0, pAll = 0;
        for (int k = 1; k <= binTop; ++k) {
            const double p = std::norm(scratch[k]);
            pAll += p;
            if (k < binLow) pLow += p;
            if (k >= binSpLo && k <= binSpHi) pSpeech += p;
        }
        const double inv = pAll > 1e-20 ? 1.0 / pAll : 0.0;
        lowFrac[fr] = pLow * inv;
        spFrac[fr]  = pSpeech * inv;

        for (int k = 0; k < N2; ++k) scratch[k] = cf((float)std::norm(scratch[k]), 0.0f);
        fft(scratch, true);

        // Harmonicity: strongest interior local maximum of the de-biased ACF.
        const double r0 = scratch[0].real();
        double best = 0.0;
        if (r0 > 1e-12) {
            for (int lag = lagMin + 1; lag < lagMax; ++lag) {
                const double a0 = scratch[lag - 1].real(), a1 = scratch[lag].real(),
                             a2 = scratch[lag + 1].real();
                if (!(a1 > a0 && a1 >= a2)) continue;
                const double bias = wac[lag] / wac0;
                if (bias <= 0.05) continue;
                const double v = a1 / (r0 * bias);
                if (v > best) best = v;
            }
        }
        harm[fr] = std::max(0.0, std::min(1.0, best));
    }

    // Loud-frame reference level (95th percentile) → relative silence gate.
    std::vector<double> sorted = rms;
    std::sort(sorted.begin(), sorted.end());
    const double loud = sorted[(size_t)((sorted.size() - 1) * 95 / 100)];
    const double levelThr = std::max(loud * (0.010 + 0.060 * s), 2e-5);

    const double harmThr = 0.12 + 0.33 * s;    // 0.12 (lenient) .. 0.45 (strict)
    const double lowThr  = 0.85 - 0.10 * s;    // bump signature: mostly < 150 Hz
    const double spThr   = 0.06 + 0.10 * s;    // speech always has mid-band energy

    std::vector<uint8_t> core(nFrames, 0);
    for (size_t fr = 0; fr < nFrames; ++fr)
        core[fr] = (rms[fr] >= levelThr && harm[fr] >= harmThr &&
                    lowFrac[fr] <= lowThr && spFrac[fr] >= spThr) ? 1 : 0;

    const double hopSec = (double)kVIHop / rate;
    auto frames = [&](double ms) { return (int)std::ceil(ms / 1000.0 / hopSec); };

    // Drop runs too short to be an utterance (rings, clicks, tonal knocks).
    const int minRun = std::max(1, frames(60.0));
    for (size_t i = 0; i < nFrames; ) {
        if (!core[i]) { ++i; continue; }
        size_t j = i; while (j < nFrames && core[j]) ++j;
        if ((int)(j - i) < minRun) for (size_t k = i; k < j; ++k) core[k] = 0;
        i = j;
    }

    // Frames that are unmistakably a non-voice *event*, so the growth below must
    // not swallow them. Pre-roll / hold / gap-merge exist to protect the quiet,
    // ambiguous material around speech — trailing consonants, breath, the tail of
    // a word — but they were also shielding loud thumps that land within a couple
    // of hundred milliseconds of a sentence, which is exactly where a bumped desk
    // or a door slam usually falls. Such a bump was left untouched while an
    // identical one in the middle of a silence was removed, so on a take where the
    // bumps cluster around the speech the effect looked like it did nothing.
    //
    // The test is deliberately narrow — audible, energy overwhelmingly below
    // 150 Hz, and next to no speech-band content — because it *overrides* the
    // protections: it must fire on thumps and never on a plosive or a breath,
    // both of which carry mid-band energy. It identifies a bump rather than
    // grading one, so it isn't tied to the sensitivity knob.
    std::vector<uint8_t> veto(nFrames, 0);
    for (size_t fr = 0; fr < nFrames; ++fr)
        veto[fr] = (!core[fr] && rms[fr] >= levelThr &&
                    lowFrac[fr] >= 0.90 && spFrac[fr] <= 0.5 * spThr) ? 1 : 0;
    // Sustained events only, so one odd frame can't punch a hole through a word.
    const int minVeto = std::max(1, frames(40.0));
    for (size_t i = 0; i < nFrames; ) {
        if (!veto[i]) { ++i; continue; }
        size_t j = i; while (j < nFrames && veto[j]) ++j;
        if ((int)(j - i) < minVeto) for (size_t k = i; k < j; ++k) veto[k] = 0;
        i = j;
    }

    // Grow: pre-roll (onsets) + hold (trailing consonants / breath).
    const int pre  = frames(120.0);
    const int post = std::max(0, frames(std::max(0.0f, opts.holdMs)));
    mask.assign(nFrames, 0);
    for (size_t i = 0; i < nFrames; ++i) {
        if (!core[i]) continue;
        const size_t a = (size_t)std::max<int64_t>(0, (int64_t)i - pre);
        const size_t b = std::min(nFrames, i + (size_t)post + 1);
        for (size_t k = a; k < b; ++k) mask[k] = 1;
    }

    // Merge short gaps so words aren't chopped mid-utterance.
    const int mergeGap = frames(150.0);
    for (size_t i = 0; i < nFrames; ) {
        if (mask[i]) { ++i; continue; }
        size_t j = i; while (j < nFrames && !mask[j]) ++j;
        if (i > 0 && j < nFrames && (int)(j - i) <= mergeGap)
            for (size_t k = i; k < j; ++k) mask[k] = 1;
        i = j;
    }

    // Growth and gap-merging never keep an identified bump (see `veto` above).
    // Applied last so it beats every protection, and only ever to frames the
    // detector did not call voice in the first place. `isolateVoice`'s gate ramp
    // still fades in and out around the result, so re-opening a hole here can't
    // click.
    for (size_t i = 0; i < nFrames; ++i) if (veto[i]) mask[i] = 0;
    return mask;
}

AudioBufferPtr isolateVoice(const AudioBuffer& buf, const VoiceIsolateOptions& opts,
                            VoiceIsolateStats* stats, int64_t statsBegin, int64_t statsEnd) {
    const int ch = std::max(1, buf.channels);
    const int64_t nf = buf.frames();
    const int rate = buf.sampleRate > 0 ? buf.sampleRate : 48000;
    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = buf.sampleRate;
    out->channels = buf.channels;
    out->samples.resize(buf.samples.size());
    if (stats) *stats = VoiceIsolateStats{};
    if (nf <= 0) return out;

    int win = kVIWin, hop = kVIHop;
    std::vector<uint8_t> mask = detectVoiceFrames(buf, opts, &win, &hop);

    // Frame mask → sample segments. Frame f is centred on f*hop + win/2, so a
    // run of frames [a,b] covers samples [a*hop + win/2, b*hop + win/2).
    struct Seg { int64_t a, b; };
    std::vector<Seg> segs;
    for (size_t i = 0; i < mask.size(); ) {
        if (!mask[i]) { ++i; continue; }
        size_t j = i; while (j < mask.size() && mask[j]) ++j;
        int64_t a = (int64_t)i * hop;
        int64_t b = (int64_t)(j - 1) * hop + win;
        if (i == 0) a = 0;
        if (j == mask.size()) b = nf;
        a = std::max<int64_t>(0, a);
        b = std::min<int64_t>(nf, b);
        // A frame's support is wider than the hop, so neighbouring runs could in
        // principle produce touching segments; coalesce instead of overlapping
        // (the walk below assumes sorted, disjoint segments).
        if (!segs.empty() && a <= segs.back().b) segs.back().b = std::max(segs.back().b, b);
        else segs.push_back({ a, b });
        i = j;
    }
    if (mask.empty()) segs.push_back({ 0, nf });   // too short to analyse: all voice

    const float g0 = (float)std::pow(10.0, -std::max(0.0f, opts.reductionDb) / 20.0);
    const int64_t fade = std::max<int64_t>(0, (int64_t)(std::max(0.0f, opts.fadeMs) * 0.001 * rate));

    // Statistics are reported over [s0,s1) only — the whole buffer by default,
    // or just the range the caller is going to keep (see blendProcessedRange).
    if (stats) {
        const int64_t s0 = std::max<int64_t>(0, statsBegin);
        const int64_t s1 = std::max(s0, statsEnd < 0 ? nf : std::min(nf, statsEnd));
        int64_t voiceFrames = 0, segCount = 0;
        for (auto& sg : segs) {
            const int64_t a = std::max(sg.a, s0), b = std::min(sg.b, s1);
            if (b > a) { voiceFrames += b - a; ++segCount; }
        }
        stats->segments = (int)segCount;
        stats->voiceSeconds = (double)voiceFrames / rate;
        stats->removedSeconds = (double)((s1 - s0) - voiceFrames) / rate;
    }

    // Walk samples with a moving segment index; outside a segment the gain ramps
    // between g0 and 1 with a raised cosine over `fade` samples on each side.
    size_t si = 0;
    const float* in = buf.samples.data();
    float* op = out->samples.data();
    for (int64_t i = 0; i < nf; ++i) {
        while (si < segs.size() && i >= segs[si].b) ++si;
        float g;
        if (si < segs.size() && i >= segs[si].a) {
            g = 1.0f;
        } else if (fade <= 0) {
            g = g0;
        } else {
            double t = 0.0;
            if (si > 0) {                                   // leaving the previous segment
                const int64_t d = i - segs[si - 1].b;
                if (d < fade) t = std::max(t, 1.0 - (double)d / fade);
            }
            if (si < segs.size()) {                         // approaching the next
                const int64_t d = segs[si].a - i;
                if (d < fade) t = std::max(t, 1.0 - (double)d / fade);
            }
            const double smooth = 0.5 - 0.5 * std::cos(PI * std::max(0.0, std::min(1.0, t)));
            g = (float)(g0 + (1.0 - g0) * smooth);
        }
        if (opts.residue) g = 1.0f - g;
        for (int c = 0; c < ch; ++c) op[i * ch + c] = in[i * ch + c] * g;
    }
    return out;
}

// ---------------- manual region edits ----------------

AudioBufferPtr silenceRange(const AudioBuffer& src, int64_t begin, int64_t end, float fadeMs) {
    const int ch = src.channels;
    const int64_t nf = src.frames();
    if (ch <= 0) return nullptr;
    begin = std::max<int64_t>(0, begin);
    end   = std::min<int64_t>(nf, end);
    if (end <= begin) return nullptr;

    auto out = std::make_shared<AudioBuffer>(src);
    const int rate = src.sampleRate > 0 ? src.sampleRate : 48000;
    // Both ramps have to fit inside the range without meeting, so a selection
    // shorter than two fades gets proportionally shorter ones rather than a
    // ramp that never reaches zero.
    int64_t fade = (int64_t)(std::max(0.0f, fadeMs) * 0.001 * rate);
    fade = std::min(fade, (end - begin) / 2);

    for (int64_t i = begin; i < end; ++i) {
        double g = 0.0;   // gain applied to the original signal
        if (fade > 0) {
            const int64_t d = std::min(i - begin, end - 1 - i);
            if (d < fade) g = 0.5 + 0.5 * std::cos(PI * (d + 0.5) / fade);
        }
        for (int c = 0; c < ch; ++c) {
            const size_t k = (size_t)i * ch + c;
            out->samples[k] = (float)(src.samples[k] * g);
        }
    }
    return out;
}

AudioBufferPtr deleteRange(const AudioBuffer& src, int64_t begin, int64_t end, float fadeMs) {
    const int ch = src.channels;
    const int64_t nf = src.frames();
    if (ch <= 0) return nullptr;
    begin = std::max<int64_t>(0, begin);
    end   = std::min<int64_t>(nf, end);
    if (end <= begin) return nullptr;

    const int64_t head = begin, tail = nf - end;
    if (head + tail <= 0) return nullptr;

    const int rate = src.sampleRate > 0 ? src.sampleRate : 48000;
    // The overlap is taken from the audio being *kept*, so it can never be
    // longer than the shorter side; deleting from the very start or end of a
    // clip leaves nothing to fade against and simply splices.
    int64_t fade = (int64_t)(std::max(0.0f, fadeMs) * 0.001 * rate);
    fade = std::min({ fade, head, tail });

    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = src.sampleRate; out->channels = ch;
    const int64_t nOut = head + tail - fade;
    out->samples.assign((size_t)nOut * ch, 0.0f);

    for (int64_t i = 0; i < head - fade; ++i)
        for (int c = 0; c < ch; ++c)
            out->samples[(size_t)i * ch + c] = src.samples[(size_t)i * ch + c];

    // Equal-power crossfade: the two sides are unrelated room tone, so summing
    // them with sin/cos weights holds the level steady where a linear fade
    // would dip.
    for (int64_t i = 0; i < fade; ++i) {
        const double t = (i + 0.5) / fade;
        const double a = std::cos(t * PI * 0.5), b = std::sin(t * PI * 0.5);
        for (int c = 0; c < ch; ++c)
            out->samples[(size_t)(head - fade + i) * ch + c] =
                (float)(src.samples[(size_t)(head - fade + i) * ch + c] * a +
                        src.samples[(size_t)(end + i) * ch + c] * b);
    }

    for (int64_t i = fade; i < tail; ++i)
        for (int c = 0; c < ch; ++c)
            out->samples[(size_t)(head - fade + i) * ch + c] =
                src.samples[(size_t)(end + i) * ch + c];

    return out;
}

// ---------------- applying a result to part of a clip ----------------

AudioBufferPtr blendProcessedRange(const AudioBuffer& original, const AudioBuffer& processed,
                                   int64_t begin, int64_t end, float blendMs) {
    const int ch = original.channels;
    const int64_t nf = original.frames();
    if (ch <= 0 || processed.channels != ch || processed.frames() != nf) return nullptr;
    begin = std::max<int64_t>(0, begin);
    end   = std::min<int64_t>(nf, end);
    if (end <= begin) return nullptr;

    auto out = std::make_shared<AudioBuffer>(original);
    const int rate = original.sampleRate > 0 ? original.sampleRate : 48000;
    // Cap the ramp at a third of the range so even a very short selection keeps
    // a symmetric fade at both edges instead of two overlapping ones.
    int64_t fade = (int64_t)(std::max(0.0f, blendMs) * 0.001 * rate);
    fade = std::min(fade, (end - begin) / 3);

    for (int64_t i = begin; i < end; ++i) {
        double w = 1.0;   // weight of the processed signal
        if (fade > 0) {
            const int64_t d = std::min(i - begin, end - 1 - i);
            if (d < fade) w = 0.5 - 0.5 * std::cos(PI * (d + 0.5) / fade);
        }
        for (int c = 0; c < ch; ++c) {
            const size_t k = (size_t)i * ch + c;
            out->samples[k] = (float)(original.samples[k] * (1.0 - w) + processed.samples[k] * w);
        }
    }
    return out;
}

} // namespace dsp
