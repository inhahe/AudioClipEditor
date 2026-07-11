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
- **Select a section** of any clip by dragging across its waveform. While a
  selection is active the card shows **Crop** and **Save sel** buttons, and the
  card's play button auditions **only the selected part** (so you can preview
  before committing).
- **Save selection as a new clip** — the **Save sel** button (or right-click →
  Save selection) slices the selection into a new clip you name (rename any time).
- **Crop a clip** to the selection — the **Crop** button (or right-click → Crop).
  You're asked whether to **overwrite the original file**, **save as a new file**
  (a Save-As dialog with file type, bitrate, and mono/stereo), or just trim it in
  the editor.
- **Undo/redo tree.** `Ctrl+Z` undoes anything. `Ctrl+Shift+Z` redoes — and when
  the history has branched, it pops up a menu of the redo branches with
  descriptions so you can pick which future to walk into.
- **Tracks & timeline.** The timeline sits on top (sized to just fit the current
  number of tracks) with the clip library below it. Start with one track, add as
  many as you like. Drag a clip card up onto a track to drop it at any time
  offset; drag placed clips to move them. Clips **snap** to butt up against
  neighbours and **can't overlap** on a track.
- **Prominent Play All** button plays every track together from the playhead.
- **Per-clip and per-track volume.** Every card and every track header has a
  volume slider (0–200%). Gain is applied live to preview, timeline mixing, and
  export, and each change is a single undo step.
- **Speech-aware normalize.** Right-click a clip to **normalize it to match the
  other clips**, or **normalize every clip on every track** to a common level.
  Loudness is measured over speech only — relatively silent gaps are ignored — so
  quiet talkers get pulled up without amplifying room tone.
- **Voice cleaner (noise reduction).** Right-click → *Voice cleaner* runs an STFT
  denoiser to strip steady background noise. Choose the **algorithm** (spectral
  subtraction or Wiener filter) and how **aggressively** to clean (light / medium
  / aggressive). The result replaces the clip's audio (undoable).
- **Save / load projects.** Projects are saved as a single `.acep` file with the
  whole library and timeline (audio embedded), so a project is fully
  self-contained. `Ctrl+S` saves; the File menu has Open / Save / Save As.
- **Export the mixdown.** *File → Export Mixdown* renders all tracks together and
  saves through the same audio dialog — pick **format, sample rate, bit depth**
  (16/24-bit PCM or 32-bit float for WAV), **bitrate** (compressed formats), and
  mono/stereo. The mix is resampled to the chosen output rate on the way out.
- **Mixed source rates are unified.** Clips can come from files of any sample rate
  or bit depth; each is resampled to the project's internal rate on load. The
  project runs at **≥ 48 kHz internally** and the engine resamples to the audio
  device on playback, so nothing plays back at the wrong pitch.
- **Menu bar** (File / Edit / Track / Help) mirrors the toolbar and adds project
  I/O, mixdown export, and a Controls reference.

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
| Play / pause a clip | green button on the card, or `Space` on the active clip (plays only the selection when one is active) |
| Seek within a clip | click the waveform |
| Make a selection | drag across the waveform |
| Crop / save a selection | **Crop** and **Save sel** buttons on the card while a selection is active |
| Clip actions | **right-click a card**: play selection, save selection as new clip, crop, normalize, voice cleaner, rename, delete, add to a track |
| Rename a clip | right-click → Rename, or double-click the card title |
| Set clip / track volume | drag the volume slider on the card or track header |
| Normalize volume | right-click a card → *Normalize to match other clips* / *Normalize all clips* |
| Clean up noise | right-click a card → *Voice cleaner…* |
| Add a track | **+ Add Track** |
| Place a clip on a track | drag a clip card up into a track lane (drops at any time offset, snaps to a neighbouring clip), or right-click → Add to timeline |
| Move a placed clip | drag it (snaps to neighbours; drag vertically to change tracks) |
| Remove a placed clip | right-click it → Remove |
| Set the playhead | click a track lane or the ruler |
| Play all tracks | **▶ Play All** (`Space` when nothing is being previewed) |
| Stop | **■ Stop** |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` (or the toolbar buttons) |
| Add files / Save project | `Ctrl+O` / `Ctrl+S` |
| Open / Save / Export | **File** menu |
| Scroll library | mouse wheel over the clip area |
| Scroll / zoom timeline | wheel = scroll, `Ctrl`+wheel = zoom, `Shift`+wheel = scroll tracks |

## Architecture

| File | Role |
|---|---|
| `audio_buffer.h` | Canonical float PCM buffer + peak (volume-graph) cache |
| `model.h` | `Clip`, `Track`, `PlacedClip`, `Project` |
| `decoder.{h,cpp}` | Media Foundation Source Reader → stereo float at the device rate |
| `encoder.{h,cpp}` | WAV (manual RIFF, 16/24-bit PCM or 32-bit float) + Media Foundation Sink Writer (MP3/AAC/WMA); resamples to the chosen output rate |
| `engine.{h,cpp}` | WASAPI render engine; `BufferSource` (preview) + `TimelineSource` (mix all tracks); linear-resamples the project rate to the device rate on the audio thread |
| `undo.h` | Snapshot-based undo **tree** with branching redo |
| `document.{h,cpp}` | Owns the project + undo tree; every edit commits a snapshot |
| `waveform.{h,cpp}` | GDI filled volume-graph rendering |
| `dsp.{h,cpp}` | FFT, speech-aware loudness, STFT noise reduction (spectral subtraction / Wiener) |
| `dialogs.{h,cpp}` | Text prompt, export-options modal, voice-cleaner options, open/save file & project dialogs |
| `ui.cpp` | Main window: menu bar, layout, painting, hit-testing, all interaction |
| `selftest.cpp` | `--selftest` backend round-trip checks (decode/encode/peaks/DSP) |

All audio is decoded to the project's internal rate (**max(48 kHz, device mix
rate)**) as stereo float, so clips of any source rate/bit depth are unified. When
the device mix rate is below the internal rate, the engine linear-resamples on
the audio thread during playback. Sample-rate / bit-depth / channel / container
conversion for output happens at export time.
