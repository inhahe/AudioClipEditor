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

// Voice cleaner (noise reduction) options modal. Returns true on Apply.
bool voiceCleaner(HWND parent, dsp::NROptions& opts);

// Project open/save file dialogs (.acep). Return empty string on cancel.
std::wstring openProject(HWND parent);
std::wstring saveProject(HWND parent, const std::wstring& suggested);

} // namespace dlg
