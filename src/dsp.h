#pragma once
#include "audio_buffer.h"
#include <cstdint>

namespace dsp {

// --- Loudness (speech-aware) ---
// RMS level of the "active" (non-silent) portion of a buffer, so pauses between
// speech don't drag the measured level down. Returns linear RMS (0..~1).
double speechLoudness(const AudioBuffer& buf);

// --- Voice cleaner (noise reduction) ---
enum class NRAlgorithm { SpectralSubtraction, Wiener };
enum class NRStrength { Light, Medium, Aggressive };

struct NROptions {
    NRAlgorithm algorithm = NRAlgorithm::SpectralSubtraction;
    NRStrength strength = NRStrength::Medium;
};

// Returns a new buffer with background noise reduced. The noise profile is
// auto-estimated from the quietest frames (assumes speech has pauses).
AudioBufferPtr denoise(const AudioBuffer& buf, const NROptions& opts);

} // namespace dsp
