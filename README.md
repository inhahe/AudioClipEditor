# Audio Clip Editor

A lightweight native-Windows audio clip editor built in C++ with the Win32 API,
**Media Foundation** (decode/encode), and **WASAPI** (low-latency mixed playback).
No external audio libraries — everything ships with Windows.

It's aimed at the simple workflow that Audacity/REAPER make fiddly: load a bunch
of clips, audition them, trim/crop, and arrange them on tracks.

## Features

- **Load any number of audio files as clips.** Supports every format Media
  Foundation can decode (WAV, MP3, M4A/AAC, FLAC, WMA, …). Clips show in a
  word-wrapped grid of cards, each with an **oscilloscope waveform** (a min/max
  envelope that turns into a real sample-accurate scope trace as you zoom in). Card widths
  are **proportional to clip length** at the same scale as the timeline, and a
  global **Scale** slider in the toolbar zooms both together.
- **Play any clip** with a per-card play/pause button. Click the waveform to
  seek / skip around. Playback is real, mixed, low-latency WASAPI.
- **Select a section** of any clip by dragging across its waveform. While a
  selection is active the card shows **Crop** and **Save sel** buttons, and the
  card's play button auditions **only the selected part** (so you can preview
  before committing).
- **Save selection as a new clip** — the **Save sel** button (or right-click →
  Save selection) slices the selection into a new clip you name (rename any time).
- **Full-window clip editor.** Double-click a clip card (or right-click
  → *Open in editor*) to blow the clip up to a full-window view for precise work.
  It has its own toolbar (play, play selection, crop, save selection, capture noise, clear, done)
  and — because dragging a pixel-precise edge on a zoomed-out waveform is fiddly —
  a pair of **fine-tune edge strips**: two zoomed lanes, one centred on the
  selection's **start** and one on its **end**, so you can nudge each boundary
  exactly. The strips appear automatically when the main view is too coarse, or any
  time via the **Fine-tune edges** toggle, and each strip **wheel-zooms** to dial in
  the window. `Esc` closes the editor.
- **Crop a clip** to the selection — the **Crop** button (or right-click → Crop).
  Cropping only trims the clip inside the editor (a single undo step); it **never
  overwrites or deletes the original file on disk**. To keep a cropped copy, use
  **Save sel** (save the selection as a new clip) or export.
- **Undo/redo tree.** `Ctrl+Z` undoes anything. `Ctrl+Shift+Z` redoes — and when
  the history has branched, it pops up a menu of the redo branches with
  descriptions so you can pick which future to walk into.
- **Tracks & timeline.** The timeline sits on top (sized to just fit the current
  number of tracks) with the clip library below it. Start with one track, add as
  many as you like — once there are more tracks than fit, the tracks pane grows a
  **vertical scrollbar** (and the wheel over the track headers scrolls them), and
  adding a track scrolls it into view. Drag a clip card up onto a track to drop it
  at any time offset; drag placed clips to move them. Clips **snap** to butt up
  against neighbours and **can't overlap** on a track.
- **Prominent Play All** button plays every track together from the playhead.
- **Per-clip and per-track volume.** Every card and every track header has a
  volume slider (0–200%). Gain is applied live to preview, timeline mixing, and
  export, and each change is a single undo step.
- **Speech-aware normalize.** Right-click a clip to **normalize it to match the
  other clips**, or **normalize every clip on every track** to a common level.
  Loudness is measured over speech only — relatively silent gaps are ignored — so
  quiet talkers get pulled up without amplifying room tone.
- **Voice cleaner (noise reduction).** Right-click → *Voice cleaner* runs an STFT
  denoiser to strip steady background noise. Three **algorithms**: automatic
  **spectral subtraction** or **Wiener filter** (noise floor estimated from the
  quietest frames; pick light / medium / aggressive), or the Audacity-style
  **noise profile** gate — a faithful reimplementation of Audacity's Noise
  Reduction effect with the same two-step workflow and the same options. Drag a
  selection over a noise-only stretch, capture it with **Get noise profile**
  (button in the dialog, or right-click → *Voice cleaner → Capture noise from
  selection*) — or capture from an **entire clip** (right-click → *Voice cleaner →
  Capture noise from whole clip*) or from **inside the full-window editor** (the
  **Capture noise** toolbar button, which uses the current selection if one is
  active, else the whole clip) — then clean any clip with **Noise reduction (dB)** (0–48),
  **Sensitivity** (0.01–24), **Frequency smoothing (bands)** (0–12), and
  **Reduce / Residue** — Residue keeps only what would be removed, so you can
  audition exactly what you're losing. You can clean at three **scopes** — a
  single clip, **every clip on one track** (right-click the track header →
  *Voice cleaner — this track*), or **every clip in the whole project** (clip menu
  → *Voice cleaner → All clips*). A track/project clean uses one set of options
  and lands as a single undo step.
- **Reusable noise captures.** Each captured noise profile is remembered as a
  **recent capture** (its computed spectral profile — the per-capture work that's
  shared across every clip it cleans — is cached, so re-using it is instant). Any
  clip's right-click *Voice cleaner → Apply noise capture ▸* lists the recent
  captures; pick one to clean that clip with it in a single click (most-recently
  used floats to the top). Captures live for the session.
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
| Open the full-window editor | **double-click** a clip card, or right-click → *Open in editor* |
| Fine-tune selection edges | in the editor, drag the **Start edge** / **End edge** strip knobs (wheel over a strip to zoom); toggle with **Fine-tune edges** |
| Close the full-window editor | `Esc` or the **Done** button |
| Crop / save a selection | **Crop** and **Save sel** buttons on the card while a selection is active |
| Clip actions | **right-click a card**: play selection, save selection as new clip, crop, normalize, voice cleaner, rename, delete, add to a track |
| Rename a clip | right-click → Rename |
| Set clip / track volume | drag the volume slider on the card or track header |
| Normalize volume | right-click a card → *Normalize to match other clips* / *Normalize all clips* |
| Clean up noise | right-click a card → *Voice cleaner → This clip / All clips*, or right-click a track header → *Voice cleaner — this track* |
| Capture a noise profile | select a noise-only stretch, then right-click → *Voice cleaner → Capture noise from selection* (or the **Get noise profile** button in the dialog); or *Capture noise from whole clip*; or the **Capture noise** button in the full-window editor |
| Apply a saved noise capture | right-click a card → *Voice cleaner → Apply noise capture ▸* and pick a recent capture |
| Track actions | **right-click a track header/lane**: voice-clean the track, rename, remove |
| Add a track | **+ Add Track** |
| Place a clip on a track | drag a clip card up into a track lane (drops at any time offset, snaps to a neighbouring clip), or right-click → Add to timeline |
| Move a placed clip | drag it along the track — a live ghost shows where it will land (snaps to neighbours; drag vertically to change tracks) |
| Remove a placed clip | right-click it → Remove |
| Set the playhead | click a track lane or the ruler |
| Play all tracks | **▶ Play All** (`Space` when nothing is being previewed) |
| Stop | **■ Stop** |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` (or the toolbar buttons) |
| Add files / Save project | `Ctrl+O` / `Ctrl+S` |
| Open / Save / Export | **File** menu |
| Scroll library | mouse wheel over the clip area |
| Scroll / zoom timeline | wheel = scroll, `Ctrl`+wheel = zoom, `Shift`+wheel (or wheel over the track headers) = scroll tracks vertically; drag the scrollbar on the right when there are more tracks than fit |
| Time scale (zoom) | the **Scale** slider in the toolbar sets pixels-per-second for both the timeline *and* the clip cards |

## Architecture

| File | Role |
|---|---|
| `audio_buffer.h` | Canonical float PCM buffer + min/max (oscilloscope) bucket cache |
| `model.h` | `Clip`, `Track`, `PlacedClip`, `Project` |
| `decoder.{h,cpp}` | Media Foundation Source Reader → stereo float at the device rate |
| `encoder.{h,cpp}` | WAV (manual RIFF, 16/24-bit PCM or 32-bit float) + Media Foundation Sink Writer (MP3/AAC/WMA); resamples to the chosen output rate |
| `engine.{h,cpp}` | WASAPI render engine; `BufferSource` (preview) + `TimelineSource` (mix all tracks); linear-resamples the project rate to the device rate on the audio thread |
| `undo.h` | Snapshot-based undo **tree** with branching redo |
| `document.{h,cpp}` | Owns the project + undo tree; every edit commits a snapshot |
| `waveform.{h,cpp}` | GDI oscilloscope rendering: min/max envelope zoomed out, sample line zoomed in |
| `dsp.{h,cpp}` | FFT, speech-aware loudness, STFT noise reduction (spectral subtraction / Wiener / Audacity-style profile gate) |
| `dialogs.{h,cpp}` | Text prompt, export-options modal, voice-cleaner options, open/save file & project dialogs |
| `ui.cpp` | Main window: menu bar, layout, painting, hit-testing, all interaction |
| `selftest.cpp` | `--selftest` backend round-trip checks (decode/encode/peaks/DSP) |

All audio is decoded to the project's internal rate (**max(48 kHz, device mix
rate)**) as stereo float, so clips of any source rate/bit depth are unified. When
the device mix rate is below the internal rate, the engine linear-resamples on
the audio thread during playback. Sample-rate / bit-depth / channel / container
conversion for output happens at export time.
