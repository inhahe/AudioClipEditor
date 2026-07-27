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
// "Range:" rows shared by the voice cleaner and remove-non-voice dialogs: let the
// user restrict the effect to the current waveform selection instead of the whole
// clip. Only meaningful when the operation targets exactly one clip, so the rows
// are omitted entirely for multi-clip scopes.
struct RangeOption {
    bool offer = false;               // single-clip scope: show the Range rows at all
    bool hasSelection = false;        // ...and there is a selection on that clip
    std::wstring selectionDesc;       // e.g. "0:12.30 - 0:18.00 (5.70 s)"
    bool selectionOnly = false;       // in/out: process only the selected range
};

struct VoiceCleanerContext {
    dsp::NROptions* opts = nullptr;               // in/out
    dsp::NoiseProfile* profile = nullptr;         // in/out (session-scoped)
    std::wstring* profileDesc = nullptr;          // in/out, e.g. "1.2 s from 'clip'"
    const AudioBuffer* noiseSelection = nullptr;  // current selection slice, or null
    std::wstring selectionDesc;                   // clip name of the selection
    bool captured = false;                        // set true if the user captured a new
                                                  // profile via Get Noise Profile, so the
                                                  // caller can add it to recent captures
    RangeOption range;                            // in/out: whole clip vs selection only
};
bool voiceCleaner(HWND parent, VoiceCleanerContext& ctx);

// Voice isolation ("remove non-voice") options modal. `opts` carries the
// last-used settings in and the chosen settings out; `scopeLabel` describes what
// the operation will be applied to (e.g. "'interview take 2'", "all clips").
// Returns true on Apply.
struct VoiceIsolateContext {
    dsp::VoiceIsolateOptions* opts = nullptr;     // in/out
    std::wstring scopeLabel;                      // "'clip'", "all clips (3 clips)"
    RangeOption range;                            // in/out: whole clip vs selection only
};
bool voiceIsolate(HWND parent, VoiceIsolateContext& ctx);

// Project open/save file dialogs (.acep). Return empty string on cancel.
std::wstring openProject(HWND parent);
std::wstring saveProject(HWND parent, const std::wstring& suggested);

} // namespace dlg
