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

// --- Timbre matching (making separately-recorded clips sound alike) ---
// Sentences recorded in separate takes drift in tone even when nothing obvious
// changed: a few centimetres of mic distance moves the low end (proximity
// effect), a few degrees off-axis rolls off the top, a different position in the
// room recolours the mids. All of that shows up as a difference in the clip's
// **long-term average spectrum** (LTAS) -- its average tone colour over the whole
// clip -- and one gentle, static EQ curve per clip is enough to bring them
// together. That is the whole idea here: measure each clip's LTAS, decide on a
// common target, and filter each clip by the difference.
//
// What it fixes: mic distance/angle, tone-control and preamp differences, dull
// vs. bright rooms. What it cannot fix, because they are not spectral-average
// differences: reverb and early reflections (a room tail is a *time* difference;
// EQ cannot add or remove one), differing background noise (use the voice
// cleaner), clipping or other distortion, and differences in delivery.

struct TimbreProfile {
    int sampleRate = 0;
    int count = 0;                  // speech frames measured (or, for an averaged
                                    // profile, how many profiles were pooled)
    std::vector<float> logPower;    // per-bin mean ln(power), bins 0..Nyquist
    bool valid() const { return count > 0 && !logPower.empty(); }
};

struct TimbreMatchOptions {
    float maxCorrectionDb = 12.0f;   // clamp on the correction curve, 0 .. 24
    float smoothingOctaves = 0.5f;   // fractional-octave smoothing width, 0.05 .. 2
    bool  preserveLoudness = true;   // restore speech loudness after filtering
};

// Measure a clip's LTAS. Only **speech** frames count: room tone differs between
// takes too, and a clip with longer pauses would otherwise be dragged toward its
// own noise floor -- matching noise floors is not what "the same timbre" means.
// Falls back to the loudest frames if the voice detector finds nothing, so an
// unusual clip is still matched rather than silently skipped. Returns an invalid
// profile for audio shorter than one 2048-frame analysis window.
TimbreProfile computeTimbreProfile(const AudioBuffer& buf);

// The consensus timbre of a set: the per-bin mean of their log spectra, i.e. the
// geometric mean of their power. Matching everything to this moves each clip as
// little as possible and favours none of them -- the same reasoning as
// Document::normalizeClips using the geometric mean of loudness. Profiles that
// are invalid, or of a different sample rate than the first valid one, are
// ignored.
TimbreProfile averageTimbre(const std::vector<TimbreProfile>& profiles);

// Filter `buf` so its long-term spectrum matches `target`. The correction is
// smoothed over `smoothingOctaves`, forced to average 0 dB, and then clamped to
// +/- maxCorrectionDb. It is applied **zero-phase** (a real, symmetric gain on
// each STFT frame), so nothing is smeared in time.
//
// Forcing the curve to average zero is deliberate: a broadband offset is
// loudness, not timbre, and leaving it in would make this quietly double as a
// normalizer and undo levels the user had already set.
//
// The curve is also faded to 0 dB outside roughly 90 Hz -- 11 kHz. Outside that
// band a speech recording is mostly its own noise floor, so the ratio between
// two clips there compares one clip's rumble and hiss to another's -- which is
// the one thing this effect is explicitly not for. Unweighted, those bins run
// away with the curve: a trace of DC offset or a fan in one room is a 20 dB
// difference at 30 Hz, far larger than any real difference in tone.
//
// `curveDbOut`, if given, receives the correction actually applied, per bin, in
// dB -- useful for reporting how far a clip had to move. Returns nullptr if the
// target is invalid, its rate doesn't match, or `buf` is too short to measure.
AudioBufferPtr matchTimbre(const AudioBuffer& buf, const TimbreProfile& target,
                           const TimbreMatchOptions& opts,
                           std::vector<float>* curveDbOut = nullptr);

// How far apart two tone colours measure, in dB -- the number that says whether
// matching actually worked. Reporting the size of the *correction* is not the
// same thing: a large correction that landed is a success and a small one that
// fell short is a failure, and only the residual distinguishes them. It also
// separates the two reasons clips still sound different after a match: a few dB
// left over means the EQ stopped short (raise the clamp), while ~0 dB left over
// means the clips now measure alike and what remains is something spectral
// averages cannot see -- reverb, compression, or background noise.
//
// Defined as the RMS of the dB difference between the two profiles over
// 100 Hz -- 10 kHz, after removing its mean (a broadband offset is loudness, not
// timbre) and weighting each bin by 1/f so every octave counts equally. The
// weighting matters: linearly-spaced bins put four fifths of their number above
// 3 kHz, so an unweighted figure would be almost entirely a statement about the
// top octave. Returns -1 if the profiles aren't comparable.
double timbreDistanceDb(const TimbreProfile& a, const TimbreProfile& b);

// Hz per bin of a TimbreProfile / of the curve `matchTimbre` reports, so a
// caller can say *where* the largest correction landed. "8 dB at 250 Hz" and
// "8 dB at 40 Hz" mean completely different things.
double timbreBinHz(int sampleRate);

// --- Manual region edits ---
// The detectors above are the automatic route, but they can only remove what
// they can recognise. Some non-speech events -- a chair creak, a swallow, a
// shuffle that happens to ring -- are harmonic, mid-band and syllable-length,
// which is to say they look exactly like voiced speech to any per-frame feature
// test; no sensitivity setting separates them. These two functions are the
// manual fallback for that material: the user hears it, selects it, and says so.

// Silence [begin, end), ramping down over the first `fadeMs` of the range and
// back up over the last `fadeMs` so the edit can't click. The ramps live
// *inside* the selection, so audio outside it is bit-identical and the clip's
// length and timing are unchanged. Returns nullptr for an empty range.
AudioBufferPtr silenceRange(const AudioBuffer& src, int64_t begin, int64_t end, float fadeMs = 5.0f);

// Cut [begin, end) out and close the gap, equal-power crossfading across the
// join so the splice can't click. The crossfade overlaps `fadeMs` of the audio
// *kept* on either side (never the removed material, which would defeat the
// point), so the result is `fadeMs` shorter than a plain concatenation.
// Returns nullptr for an empty range or if nothing would be left.
AudioBufferPtr deleteRange(const AudioBuffer& src, int64_t begin, int64_t end, float fadeMs = 5.0f);

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
