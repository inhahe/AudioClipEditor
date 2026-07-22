#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "encoder.h"
#include "dsp.h"

namespace dlg {

// Simple modal text prompt (used for naming / renaming clips). Returns true on OK.
bool promptText(HWND parent, const wchar_t* title, const wchar_t* label, std::wstring& text);

// Multi-select open dialog for audio files. Returns selected paths.
std::vector<std::wstring> openAudioFiles(HWND parent);

// Export options modal: choose format / bitrate / channels, then a save-file dialog.
// Fills opts + outPath. Returns true on OK.
bool exportOptions(HWND parent, mfio::ExportOptions& opts, std::wstring& outPath,
                   const std::wstring& suggestedName);

// Voice cleaner (noise reduction) options modal. `opts` carries the last-used
// settings in and the chosen settings out (the caller persists them for the
// session). For the Audacity-style "Noise profile" algorithm the dialog offers
// a Get Noise Profile button that captures `*profile` from `noiseSelection`
// (the current waveform selection, may be null) and describes it in
// `*profileDesc`. Returns true on Apply.
struct VoiceCleanerContext {
    dsp::NROptions* opts = nullptr;               // in/out
    dsp::NoiseProfile* profile = nullptr;         // in/out (session-scoped)
    std::wstring* profileDesc = nullptr;          // in/out, e.g. "1.2 s from 'clip'"
    const AudioBuffer* noiseSelection = nullptr;  // current selection slice, or null
    std::wstring selectionDesc;                   // clip name of the selection
    bool captured = false;                        // set true if the user captured a new
                                                  // profile via Get Noise Profile, so the
                                                  // caller can add it to recent captures
};
bool voiceCleaner(HWND parent, VoiceCleanerContext& ctx);

// Project open/save file dialogs (.acep). Return empty string on cancel.
std::wstring openProject(HWND parent);
std::wstring saveProject(HWND parent, const std::wstring& suggested);

} // namespace dlg
