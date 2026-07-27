#pragma once
#include "audio_buffer.h"
#include <cstdint>
#include <vector>

namespace dsp {

// --- Loudness (speech-aware) ---
// RMS level of the "active" (non-silent) portion of a buffer, so pauses between
// speech don't drag the measured level down. Returns linear RMS (0..~1).
double speechLoudness(const AudioBuffer& buf);

// --- Voice cleaner (noise reduction) ---
enum class NRAlgorithm { SpectralSubtraction, Wiener, Profile };
enum class NRStrength { Light, Medium, Aggressive };

// --- Audacity-style profile-based noise reduction (spectral noise gating) ---
// Faithful reimplementation of Audacity's "Noise Reduction" effect in its
// released configuration: 2048-sample Hann/Hann STFT at 4 steps per window,
// "second greatest" noise classification over 5 consecutive windows,
// exponential attack (0.02 s) / release (0.10 s) gain smoothing, and
// geometric-mean frequency smoothing of the gain curve. Two-step workflow:
// capture a NoiseProfile from noise-only audio, then reduce.

struct NoiseProfile {
    int sampleRate = 0;          // audio to be cleaned must match this rate
    int windows = 0;             // complete STFT windows accumulated
    double seconds = 0.0;        // length of the profiled audio (UI display)
    std::vector<float> means;    // per-bin mean noise power (all channels pooled)
    bool valid() const { return windows > 0 && !means.empty(); }
};

// Same options (names, ranges, defaults) as Audacity 3.x's dialog.
struct NRProfileOptions {
    float reductionDb = 6.0f;      // "Noise reduction (dB)"           0 .. 48
    float sensitivity = 6.0f;      // "Sensitivity"                 0.01 .. 24
    int   freqSmoothingBands = 6;  // "Frequency smoothing (bands)"    0 .. 12
    bool  residue = false;         // "Noise:" Reduce (false) / Residue (true)
};

struct NROptions {
    NRAlgorithm algorithm = NRAlgorithm::SpectralSubtraction;
    NRStrength strength = NRStrength::Medium;      // SpectralSubtraction / Wiener
    NRProfileOptions profile;                      // Profile algorithm settings
    const NoiseProfile* noiseProfile = nullptr;    // required when algorithm == Profile
};

// "Get Noise Profile": accumulate per-frequency noise statistics from a
// noise-only buffer (every complete window of every channel contributes, as in
// Audacity). Returns an invalid profile (windows == 0) when the buffer is
// shorter than one STFT window (2048 frames).
NoiseProfile computeNoiseProfile(const AudioBuffer& buf);

// Apply profile-based noise reduction. Returns nullptr if the profile is
// invalid or its sample rate does not match the buffer (mirrors Audacity's
// "sample rate of the noise profile must match" rule).
AudioBufferPtr denoiseWithProfile(const AudioBuffer& buf, const NoiseProfile& profile,
                                  const NRProfileOptions& opts);

// Returns a new buffer with background noise reduced. For the auto algorithms
// (SpectralSubtraction / Wiener) the noise profile is estimated from the
// quietest frames (assumes speech has pauses). For NRAlgorithm::Profile the
// captured opts.noiseProfile is used (nullptr / mismatched rate -> nullptr).
AudioBufferPtr denoise(const AudioBuffer& buf, const NROptions& opts);

// --- Voice isolation (remove everything that isn't speech) ---
// Where noise reduction attacks *steady* background noise, this attacks
// *non-speech events*: bumps, thumps, chair/paper shuffling, door slams, clicks,
// keyboard, and the room tone between sentences. Speech is detected per analysis
// frame (harmonic structure + speech-band energy + level), the detections are
// grown to cover consonants and merged across short gaps, and everything outside
// the resulting voice segments is attenuated. The clip's length and timing are
// never changed — non-voice is silenced in place, not cut out.
struct VoiceIsolateOptions {
    float sensitivity = 0.5f;    // 0 = keep more (lenient) .. 1 = remove more (strict)
    float reductionDb = 60.0f;   // attenuation applied to non-voice, 0 .. 96 (96 ~ silence)
    float holdMs      = 200.0f;  // keep the gate open this long after voice stops, 0 .. 2000
    float fadeMs      = 25.0f;   // gate ramp length, 0 .. 500 (avoids clicks)
    bool  residue     = false;   // keep only what would be removed (audition what's lost)
};

struct VoiceIsolateStats {
    double voiceSeconds   = 0.0;  // audio kept at full gain
    double removedSeconds = 0.0;  // audio attenuated
    int    segments       = 0;    // number of detected voice segments
};

// Returns a new buffer with non-voice attenuated (or, with `residue`, only the
// removed material). Never returns nullptr for a non-empty buffer: audio too
// short to analyse (< one 2048-frame window) is treated as all-voice.
//
// `statsBegin`/`statsEnd` restrict the *reported statistics* to a frame range
// (default: the whole buffer); `statsEnd < 0` means "to the end". The processing
// itself always covers the whole buffer — see blendProcessedRange for applying
// the result to only part of a clip, which is what this range accompanies.
AudioBufferPtr isolateVoice(const AudioBuffer& buf, const VoiceIsolateOptions& opts,
                            VoiceIsolateStats* stats = nullptr,
                            int64_t statsBegin = 0, int64_t statsEnd = -1);

// Frame-level voice mask, one flag per hop (frame f covers samples
// [f*hop, f*hop + win)). Exposed for tests / future visualisation.
std::vector<uint8_t> detectVoiceFrames(const AudioBuffer& buf, const VoiceIsolateOptions& opts,
                                       int* winOut = nullptr, int* hopOut = nullptr);

// --- Applying an effect to only part of a clip ---
// Returns a copy of `original` in which [begin, end) is taken from `processed`,
// crossfaded over `blendMs` at each edge so the join between processed and
// untouched audio can't click.
//
// `processed` must be the *whole-buffer* result of the effect. Running the
// effect on the whole clip and then keeping only part of the result — rather
// than slicing first and processing the slice — is deliberate: the auto noise
// estimators pick their noise floor from the quietest frames and the voice
// detector needs surrounding context, so a short slice analysed in isolation
// would give a different (and usually much worse) result. This way the audio
// inside the range is exactly what a whole-clip run would have produced there.
//
// Returns nullptr if the buffers disagree on length / channel count, or if the
// range is empty after clamping.
AudioBufferPtr blendProcessedRange(const AudioBuffer& original, const AudioBuffer& processed,
                                   int64_t begin, int64_t end, float blendMs = 5.0f);

} // namespace dsp
