# Audio Clip Editor

A lightweight native-Windows audio clip editor built in C++ with the Win32 API,
**Media Foundation** (decode/encode), and **WASAPI** (low-latency mixed playback).
No external audio libraries — everything ships with Windows.

It's aimed at the simple workflow that Audacity/REAPER make fiddly: load a bunch
of clips, audition them, trim/crop, and arrange them on tracks.

## Features

- **Load any number of audio files as clips.** Supports every format Media
  Foundation can decode (WAV, MP3, M4A/AAC, FLAC, WMA, …). Clips show in a
  word-wrapped grid of cards, each with a **volume-graph waveform**.
- **Play any clip** with a per-card play/pause button. Click the waveform to
  seek / skip around. Playback is real, mixed, low-latency WASAPI.
- **Select a section** of any clip by dragging across its waveform. Preview the
  selection (▶ Play selection) before committing, then **Save selection as a new
  clip** (you name it; rename any time).
- **Crop a clip** to the selection. You're asked whether to **overwrite the
  original file**, **save as a new file** (a Save-As dialog with file type,
  bitrate, and mono/stereo), or just trim it in the editor.
- **Undo/redo tree.** `Ctrl+Z` undoes anything. `Ctrl+Shift+Z` redoes — and when
  the history has branched, it pops up a menu of the redo branches with
  descriptions so you can pick which future to walk into.
- **Tracks & timeline.** Start with one track, add as many as you like. Drag a
  clip's title bar onto a track to place it; drag placed clips to move them.
  Clips **snap** to butt up against neighbours and **can't overlap** on a track.
- **Prominent Play All** button plays every track together from the playhead.

## Building

Requires Visual Studio 2022 (MSVC) + Windows 10 SDK and CMake.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is written to `bin/AudioClipEditor.exe`.

Run the headless backend self-test (decode/encode/peaks round-trip) with:

```sh
bin/AudioClipEditor.exe --selftest    # writes bin/selftest.log
```

## Controls

| Action | How |
|---|---|
| Add clips | **+ Add Files** (multi-select) |
| Play / pause a clip | green button on the card, or `Space` on the active clip |
| Seek within a clip | click the waveform |
| Make a selection | drag across the waveform |
| Clip actions | **right-click a card**: play selection, save selection as new clip, crop, rename, delete, add to a track |
| Rename a clip | right-click → Rename, or double-click the card title |
| Add a track | **+ Add Track** |
| Place a clip on a track | drag the card's **title bar** into a track lane, or right-click → Add to timeline |
| Move a placed clip | drag it (snaps to neighbours; drag vertically to change tracks) |
| Remove a placed clip | right-click it → Remove |
| Set the playhead | click a track lane or the ruler |
| Play all tracks | **▶ Play All** (`Space` when nothing is being previewed) |
| Stop | **■ Stop** |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` (or the toolbar buttons) |
| Scroll library | mouse wheel over the clip area |
| Scroll / zoom timeline | wheel = scroll, `Ctrl`+wheel = zoom, `Shift`+wheel = scroll tracks |

## Architecture

| File | Role |
|---|---|
| `audio_buffer.h` | Canonical float PCM buffer + peak (volume-graph) cache |
| `model.h` | `Clip`, `Track`, `PlacedClip`, `Project` |
| `decoder.{h,cpp}` | Media Foundation Source Reader → stereo float at the device rate |
| `encoder.{h,cpp}` | WAV (manual RIFF) + Media Foundation Sink Writer (MP3/AAC/WMA) |
| `engine.{h,cpp}` | WASAPI render engine; `BufferSource` (preview) + `TimelineSource` (mix all tracks) |
| `undo.h` | Snapshot-based undo **tree** with branching redo |
| `document.{h,cpp}` | Owns the project + undo tree; every edit commits a snapshot |
| `waveform.{h,cpp}` | GDI filled volume-graph rendering |
| `dialogs.{h,cpp}` | Text prompt + export-options modal + open/save file dialogs |
| `ui.cpp` | Main window: layout, painting, hit-testing, all interaction |
| `selftest.cpp` | `--selftest` backend round-trip checks |

All audio is decoded to the output device's mix sample rate and stereo float, so
playback needs no resampling. Channel/format conversion (mono downmix, bitrate,
container) happens only at export time.
