#pragma once
#include <windows.h>
#include <functional>
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

// The two matchers (timbre and volume) make clips alike rather than processing
// each on its own, so besides *which* clips (chosen in the menu, shown in the
// "Applies to:" line) their dialogs choose *what to match them to*: the average
// (which moves each clip the least and favours none), or one clip set as the
// standard ("make everything sound like this one"). Both dialogs show that choice
// the same way, from this.
struct MatchReference {
    std::wstring averageLabel;               // the first item: "The average of them all"
    std::vector<std::wstring> clipNames;     // the clips that may be the standard, in order
    std::vector<int> clipIds;                // parallel to clipNames: keeps the user's pick
                                             // when the list is rebuilt for other tracks
    int choice = -1;                         // in/out: -1 = the average, else into clipNames
};

// The tracks a match takes its clips from, as ticks the user can change -- so a
// music or effects track can be left out of "all tracks", or a clip matched to the
// clips on two tracks rather than one. Empty `labels`: no tick-list (a library-wide
// match, which isn't about tracks).
struct MatchTracks {
    std::vector<std::wstring> labels;        // one per track, e.g. "Narration (8 clips)"
    std::vector<uint8_t> ticked;             // in/out, parallel to labels
    // Called whenever the ticks change, so the rest of the dialog follows: fills
    // `ref` and `scopeLabel` for the newly ticked tracks, and returns whether there
    // is anything to match (Apply is disabled while there isn't).
    std::function<bool(const std::vector<uint8_t>& ticked, MatchReference& ref,
                       std::wstring& scopeLabel)> rebuild;
};

// Timbre matching options modal: which tracks, the reference, and how far and how
// finely the EQ may move. Returns true on Apply.
struct TimbreMatchContext {
    dsp::TimbreMatchOptions* opts = nullptr;   // in/out (session-scoped)
    bool* alsoVolume = nullptr;                // in/out: also match volume, in the same step
                                               // (null: the option isn't offered)
    MatchReference ref;
    MatchTracks tracks;
    std::wstring scopeLabel;                   // in/out: what Apply will act on
};
bool timbreMatch(HWND parent, TimbreMatchContext& ctx);

// Volume matching modal: which tracks, and the reference. Returns true on Apply.
struct VolumeMatchContext {
    MatchReference ref;
    MatchTracks tracks;
    std::wstring scopeLabel;                   // in/out: what Apply will act on
};
bool volumeMatch(HWND parent, VolumeMatchContext& ctx);

// History window: everything done to the project, oldest first, with the steps
// that are currently in effect distinguished from the ones that were undone.
// Effects such as timbre matching or noise reduction change how clips *sound*
// without changing anything on screen, so after a few undos there is otherwise no
// way to tell whether one is still applied. Clicking a row jumps the project to
// that state, which is the same thing as pressing undo or redo the right number
// of times.
struct HistoryItem {
    std::wstring desc;      // "Match timbre of 23 clips to their average"
    bool applied = false;   // in effect right now
    bool current = false;   // the state the project is in
    bool saved = false;     // the state the file on disk holds
    int  branches = 0;      // redo children; >1 means there are unlisted alternatives
    bool restorable = true; // false: named but its state was not stored (the project
                            // file's history budget ran out), so it is a record of
                            // what happened rather than a place to go back to
};
struct HistoryContext {
    std::vector<HistoryItem> items;
    // Called when the user picks a row. The caller moves the project to that state
    // and returns the index that is current afterwards (unchanged if it refused).
    std::function<int(int)> jump;
};
void history(HWND parent, HistoryContext& ctx);

// Project open/save file dialogs (.acep). Return empty string on cancel.
std::wstring openProject(HWND parent);
std::wstring saveProject(HWND parent, const std::wstring& suggested);

} // namespace dlg
