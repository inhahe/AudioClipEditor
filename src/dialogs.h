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
//
// Both effect dialogs show an "Applies to:" line built by the caller, because the
// *what* is chosen in the right-click menu before the dialog opens (this clip /
// the selection in this clip / all clips) and the dialog only chooses the *how*.
// e.g. "'interview take 2'", "the selection in 'take 2' (0:12.30 - 0:18.00, 5.70 s)",
// "all clips (3 clips)".
struct VoiceCleanerContext {
    dsp::NROptions* opts = nullptr;               // in/out
    dsp::NoiseProfile* profile = nullptr;         // in/out (session-scoped)
    std::wstring* profileDesc = nullptr;          // in/out, e.g. "1.2 s from 'clip'"
    const AudioBuffer* noiseSelection = nullptr;  // current selection slice, or null
    std::wstring selectionDesc;                   // clip name of the selection
    std::wstring scopeLabel;                      // what Apply will act on
    bool captured = false;                        // set true if the user captured a new
                                                  // profile via Get Noise Profile, so the
                                                  // caller can add it to recent captures
};
bool voiceCleaner(HWND parent, VoiceCleanerContext& ctx);

// Voice isolation ("remove non-voice") options modal. `opts` carries the
// last-used settings in and the chosen settings out. Returns true on Apply.
struct VoiceIsolateContext {
    dsp::VoiceIsolateOptions* opts = nullptr;     // in/out
    std::wstring scopeLabel;                      // what Apply will act on
};
bool voiceIsolate(HWND parent, VoiceIsolateContext& ctx);

// Timbre matching options modal. Unlike the two effects above this one is a
// *set* operation -- it makes several clips sound alike -- so the dialog also
// picks the reference every clip is matched to: the average of the set (which
// moves each clip the least and favours none), or one named clip ("make
// everything sound like this one"). `reference` is an index into `clipNames`,
// or -1 for the average. Returns true on Apply.
struct TimbreMatchContext {
    dsp::TimbreMatchOptions* opts = nullptr;   // in/out (session-scoped)
    std::vector<std::wstring> clipNames;       // the clips being matched, in order
    int reference = -1;                        // in/out: -1 = average, else index
    std::wstring scopeLabel;                   // what Apply will act on
};
bool timbreMatch(HWND parent, TimbreMatchContext& ctx);

// Project open/save file dialogs (.acep). Return empty string on cancel.
std::wstring openProject(HWND parent);
std::wstring saveProject(HWND parent, const std::wstring& suggested);

} // namespace dlg
