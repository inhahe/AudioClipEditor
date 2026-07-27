#pragma once
#include "audio_buffer.h"
#include <string>

namespace mfio {

enum class ExportFormat { WAV, MP3, AAC, WMA };

struct ExportOptions {
    ExportFormat format = ExportFormat::WAV;
    int channels = 2;           // 1 = mono, 2 = stereo
    int sampleRate = 48000;     // output rate; buffer is resampled to this
    int bitsPerSample = 16;     // WAV only: 16, 24 (int PCM) or 32 (IEEE float)
    int bitrateKbps = 192;      // for compressed formats
};

const wchar_t* extensionFor(ExportFormat f);   // ".wav" etc.
const wchar_t* labelFor(ExportFormat f);       // "WAV (PCM)" etc.

// Turn free text (a clip or project name) into something a save dialog can be
// pre-filled with: characters a path can't hold become '_', and trailing dots /
// spaces are dropped because Windows discards them silently, which would create
// a file under a different name than the dialog displayed. Never returns empty
// — a name made entirely of illegal characters falls back to `fallback`.
std::wstring safeFileName(const std::wstring& s, const std::wstring& fallback = L"clip");

// Encode a canonical stereo-float buffer to disk with the given options
// (downmixing to mono / re-encoding as needed). Returns false + message on error.
bool encodeFile(const std::wstring& path, const AudioBuffer& buf,
                const ExportOptions& opts, std::wstring* err = nullptr);

} // namespace mfio
