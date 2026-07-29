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
- **Rearrange the clip library.** Drag a card by its title bar and drop it in a
  new spot to reorder the grid (a caret shows where it will land), or **right-click
  the empty library area** to sort every clip **by name (A–Z)** or **by time
  (oldest first)** — the time being the source file's date, or when a derived clip
  was created. Reorders and sorts are each a single undo step.
- **Play any clip** with a per-card play/pause button. Click the waveform to
  seek / skip around — the yellow cursor is always where playback will pick up,
  so clicking while paused (or stopped) moves the restart point. Pause always
  pauses, including in the middle of a selection audition, and play continues
  from where it stopped. Playback is real, mixed, low-latency WASAPI, and the
  cursor follows the audio you are **actually hearing** rather than the audio
  already handed to the sound card, moving smoothly at the screen refresh rate
  instead of hopping in lumps — so it lines up with the waveform under it even at
  the fine-tune strips' zoom.
- **Select a section** of any clip by dragging across its waveform. While a
  selection is active the card shows **Crop** and **New clip** buttons, and the
  card's play button auditions **only the selected part** (so you can preview
  before committing). Once you have a selection you can **adjust either edge on its
  own** — grab the left or right boundary and drag it (the cursor turns into a
  ↔ resize arrow when you're near an edge) instead of redrawing the whole
  selection. This works on the clip cards, in the full-window editor's main view,
  and via the editor's fine-tune edge strips. A selection is an **editing cursor**
  — it says what the next operation applies to — not a trim: a clip already placed
  on a track always plays in full, whatever is selected. To put only part of a clip
  into an arrangement, use **New clip** and drag the resulting clip onto the track.
- **Make a new clip out of a selection** — the **New clip** button on the card
  (or right-click → *New clip from selection*, or the same button in the
  full-window editor) slices the selection into a separate library clip you name,
  leaving the original untouched. This is the intended way to get "just this bit"
  of a recording as something you can arrange, rename, clean up, or export on its
  own. Note it makes a *clip*, not a file — for a file, use *Export selection…*.
- **Full-window clip editor.** Double-click a clip card (or right-click
  → *Open in editor*) to blow the clip up to a full-window view for precise work.
  It has its own toolbar (play, play selection, crop, save selection, capture noise, clear, done).
  **Play** and **Play selection** are two independent transports: each pauses only
  what it started, each turns into its own **Pause** button while it is the one
  running, and pressing one while the other is playing switches straight over — so
  the button you press is always the button that reacts. `Space` is the universal
  play/pause and acts on whatever is armed. Because dragging a pixel-precise edge
  on a zoomed-out waveform is fiddly, the editor also has
  a pair of **fine-tune edge strips**: two zoomed lanes, one centred on the
  selection's **start** and one on its **end**, so you can nudge each boundary
  exactly. The strips appear automatically when the main view is too coarse, or any
  time via the **Fine-tune edges** toggle, and each strip **wheel-zooms** to dial in
  the window. The play cursor is drawn in the strips too whenever playback passes
  through their window, so you can watch (and hear) exactly where an edge falls
  relative to the audio. `Esc` closes the editor.
- **Crop a clip** to the selection — the **Crop** button (or right-click → Crop).
  Cropping only trims the clip inside the editor (a single undo step); it **never
  overwrites or deletes the original file on disk**. To keep a cropped copy, use
  **New clip** (makes a new clip from the selection) or export.
- **Undo/redo tree.** `Ctrl+Z` undoes anything. `Ctrl+Shift+Z` redoes — and when
  the history has branched, it pops up a menu of the redo branches with
  descriptions so you can pick which future to walk into. Selections are undoable
  too: `Ctrl+Z` first walks back through the selections you've made since the last
  edit, so a stray click that wipes a carefully-placed selection costs one
  keystroke, and only once those run out does it undo the edit itself.
- **Tracks & timeline.** The timeline sits on top (sized to just fit the current
  number of tracks) with the clip library below it. Start with one track, add as
  many as you like — once there are more tracks than fit, the tracks pane grows a
  **vertical scrollbar** (and the wheel over the track headers scrolls them), and
  adding a track scrolls it into view. An arrangement wider than the window grows a
  **horizontal scrollbar** to match, and can also be scrolled with the wheel or the
  `←`/`→`/`Home`/`End` keys. Drag a clip card up onto a track to drop it
  at any time offset; drag placed clips to move them. Clips **can't overlap** on a
  track, and they **snap** to butt up flush against a neighbour — but only in the
  direction that doesn't get in your way. Sliding *toward* a neighbour never pulls,
  so you can leave a gap as small as you like; nudge *past* it and the clip lands
  exactly flush and stays there until you drag properly clear. So flush placement
  needs no aim, and every other position is still reachable.
- **Change the space between two clips without disturbing the rest.** Moving a
  clip normally eats into one gap and opens up the next, which wrecks the timing
  of everything downstream. **Shift+drag** a placed clip instead and it carries
  every clip after it on that track along with it: exactly one gap changes and
  all the later spacing is preserved. To set a gap exactly rather than by eye,
  right-click a placed clip → **Space before this clip…** and type it in seconds
  (or **Close the space before this clip** to butt it up against its
  predecessor) — the clips after it move to match either way. Sliding left stops
  when the clip is flush against the one in front; sliding right is unlimited.
  You can press Shift after you've already started dragging and the drag turns
  into a ripple there and then — and once it has, letting go of Shift won't undo
  it (press Esc if you want to abandon the drag). The clip you're dragging shows
  what it's about to do: `move • +0.42 s`, or `ripple • gap 1.35 s • carrying 4
  clips` — and an ordinary drag reminds you the other gesture is there. If you're
  doing a lot of this, tick **Timeline ▸ Ripple drag** and every clip drag carries
  the later clips with no modifier at all; Shift then means "just this one clip".
- **The view follows the playhead** while the timeline plays, so a long
  arrangement doesn't just run off the right-hand edge. It pages ahead when the
  playhead nears the edge rather than scrolling continuously, which keeps the
  waveforms readable. Scrolling by hand during playback stops it following (you
  went to look at something on purpose); pressing play again, or clicking to move
  the playhead, re-arms it.
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
- **Remove non-voice.** Where the voice cleaner attacks *steady* noise, this
  attacks *non-speech events*: right-click a clip → *Remove non-voice* to silence
  bumps, thumps, chair and paper shuffling, door slams, keyboard clicks — and the
  room tone between sentences — leaving only the speech. Each analysis frame is
  scored for harmonic (voiced) structure, speech-band energy and level; the
  detections are then grown to catch the consonants around them and merged across
  short gaps, so words aren't chopped. Everything outside a voice segment is faded
  down, but the clip's **length and timing never change** — non-voice is
  attenuated in place, not cut out. Pick **Sensitivity** (Gentle / Balanced /
  Strict), **Attenuation** (0–96 dB), **Hold after speech** and **Fade**, or
  switch the output to **Preview removed** to audition exactly what would be taken
  away. Same three scopes as the voice cleaner — this clip, every clip on one
  track (track-header right-click), or the whole project — each a single undo step.
- **Match timbre across clips.** Sentences recorded in separate takes rarely sound
  quite alike, even when nothing obvious changed: a few centimetres of mic
  distance moves the low end, a few degrees off-axis rolls off the top, a
  different spot in the room recolours the mids. Right-click a clip → **Match
  timbre across all clips…**, or a track header → **Match timbre — this track…**,
  and every clip is measured and gently EQ'd toward a common tone colour. Choose
  what to match to: **the average of them all** (the default — it moves each clip
  as little as possible and favours none) or one particular clip
  (*Sound like 'take 2'*), when you know which take sounds right. **Maximum
  change** caps how far any clip may be moved (default 12 dB), **Smoothing**
  controls how broad the correction curve is, and *Keep each clip's loudness*
  makes sure this changes only tone, never level. One undo step for the whole set.

  The correction only covers the range speech actually occupies (about
  90 Hz – 11 kHz). Below and above that a recording is mostly its own rumble and
  hiss, and matching one clip's rumble to another's is both pointless and, if a
  room has a fan in it, capable of swamping the real correction entirely.

  **The result box tells you whether it worked**, not just what it did: it names
  the two clips that were furthest apart in tone and how far apart they were, then
  the two that are furthest apart *afterwards* — say *7.2 dB apart before, 0.9 dB
  after*. That second number is the one to read. It also says at what frequency
  the largest correction landed, because *8 dB at 250 Hz* is a mic-distance
  difference and *8 dB at 40 Hz* is a fan. If it's still a few dB, the EQ
  was held back: raise **Maximum change**, or lower **Smoothing** so the
  correction can follow finer detail. If it's near zero, the clips now measure
  alike, and anything you can still hear is not a tone-colour difference at all —
  it can't fix **reverb** (a room tail is a difference in *time*, not tone, and no
  EQ can add or remove one), **compression** on some clips but not others,
  differing **background noise** (use the voice cleaner), or delivery. Knowing
  which of the two you have saves re-running the same match and hoping.
- **Clean / de-noise just part of a clip.** Drag a selection, then pick
  **This clip (selection)…** — the first entry in the *Voice cleaner* and
  *Remove non-voice* submenus, right above *This clip…* and *All clips*. The range
  is chosen in the menu rather than buried in the dialog, so you can see it is an
  option before opening anything; the dialog then restates it on an
  **Applies to:** line. Everything outside the selection is left bit-for-bit
  untouched, with a short crossfade at the edges so the join is inaudible. The
  effect still *analyses* the whole clip — a few seconds of solid speech on its own
  gives an automatic noise estimator nothing quiet to work from — so what lands
  inside the selection is exactly what a whole-clip run would have put there. Handy
  for killing one door slam, or one noisy passage, without touching the rest of the
  take. Track- and project-wide runs are always whole-clip.
- **Silence or delete a selection.** *Remove non-voice* can only remove what it can
  recognise, and some noises genuinely aren't recognisable: a chair creak, a
  swallow, a shuffle that happens to ring is harmonic, mid-band and about as long
  as a syllable — which is to say it looks exactly like a spoken vowel to any
  detector, at any sensitivity. When you can hear it but the machine can't, say so
  directly: drag a selection, then right-click the clip (or use the full-window
  editor's toolbar) and pick **Silence selection (keep timing)** or
  **Delete selection (close gap)**. Silencing leaves the clip exactly as long as it
  was, so a pause between sentences stays the length it was and nothing downstream
  on the timeline shifts; deleting cuts the range out and closes the gap. Both fade
  at the edges so the edit can't click, both are a single undo step, and neither
  touches the file on disk.
- **Reusable noise captures.** Each captured noise profile is remembered as a
  **recent capture** (its computed spectral profile — the per-capture work that's
  shared across every clip it cleans — is cached, so re-using it is instant). Any
  clip's right-click *Voice cleaner → Apply noise capture ▸* lists the recent
  captures; pick one to clean that clip with it in a single click (most-recently
  used floats to the top). Captures live for the session.
- **Save / load projects.** Projects are saved as a single `.acep` file with the
  whole library and timeline (audio embedded), so a project is fully
  self-contained. Your **current selection and playhead position are saved too**,
  so reopening a project puts you back exactly where you left off (a selection
  whose clip has since gone is quietly dropped). A selection is real work, so
  changing one counts as an unsaved change — you get the `*` and the usual save
  prompt on exit, and never lose a carefully-placed selection by accident. (The
  playhead is saved but doesn't flag the project, since it moves on its own
  during playback.) `Ctrl+S` saves; the File menu has Open / Save / Save As. The
  title bar shows a `*` while there are unsaved changes, and the app **prompts you
  to save** (Save / Don't Save / Cancel) before closing or opening another project
  if the current one has unsaved edits — so you never lose work by accident.
- **Export a single clip, or just the selection.** Right-click a clip card →
  *Export clip to file…* or *Export selection to file…*, which opens the same
  audio dialog as the mixdown. Edits live in the project file and the original
  source file on disk is **never written to**, so this is how you get an edited
  (cleaned, normalized, cropped) clip back out as audio. The defaults follow the
  clip — its own sample rate, and a mono clip stays mono. (*New clip from
  selection* is the in-project counterpart: it adds a clip to the library rather
  than writing a file.)
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
| Audition the whole clip vs. the selection | in the editor, **▶ Play** and **▶ Play selection** are separate transports — each becomes its own **Pause**, and pressing one while the other plays switches over |
| Seek within a clip | click the waveform |
| Make a selection | drag across the waveform |
| Adjust one selection edge | grab the left or right edge of an existing selection and drag it (cursor shows ↔); works on cards, the editor main view, and the fine-tune strips |
| Cancel a drag in progress | `Esc` — abandons the drag and puts things back as they were when it started: a selection sweep or edge-nudge restores the previous selection, a clip drag drops nothing |
| Undo a selection | `Ctrl+Z` steps back through the selections you have made since the last edit (and `Ctrl+Shift+Z` / `Ctrl+Y` forward again); once they run out it undoes the edit itself |
| Open the full-window editor | **double-click** a clip card, or right-click → *Open in editor* |
| Fine-tune selection edges | in the editor, drag the **Start edge** / **End edge** strip knobs (wheel over a strip to zoom); toggle with **Fine-tune edges** |
| Close the full-window editor | `Esc` (when no drag is in progress) or the **Done** button |
| Crop to a selection | **Crop** button on the card while a selection is active |
| Make a new clip from a selection | **New clip** button on the card, or right-click the card → *New clip from selection* |
| Clip actions | **right-click a card**: play selection, save selection as new clip, crop, export clip / selection to a file, normalize, voice cleaner, remove non-voice, match timbre, rename, delete, add to a track |
| Rename a clip | right-click → Rename |
| Reorder clips | drag a card by its title bar to a new spot in the library (caret shows the drop point) |
| Sort clips | right-click the empty library area → *Sort clips by name* / *by time* |
| Set clip / track volume | drag the volume slider on the card or track header |
| Normalize volume | right-click a card → *Normalize to match other clips* / *Normalize all clips* |
| Clean up noise | right-click a card → *Voice cleaner → This clip / All clips*, or right-click a track header → *Voice cleaner — this track* |
| Remove bumps / shuffling (keep only voice) | right-click a card → *Remove non-voice → This clip / All clips*, or right-click a track header → *Remove non-voice — this track* |
| Make separately-recorded clips sound alike | right-click a card → *Match timbre across all clips…*, or right-click a track header → *Match timbre — this track…* |
| Clean / de-noise only part of a clip | drag a selection, then right-click a card → *Voice cleaner* / *Remove non-voice* → **This clip (selection)…** |
| Get rid of a noise *Remove non-voice* won't touch | drag a selection over it, then right-click a card (or use the editor toolbar) → *Silence selection (keep timing)* / *Delete selection (close gap)* |
| Capture a noise profile | select a noise-only stretch, then right-click → *Voice cleaner → Capture noise from selection* (or the **Get noise profile** button in the dialog); or *Capture noise from whole clip*; or the **Capture noise** button in the full-window editor |
| Apply a saved noise capture | right-click a card → *Voice cleaner → Apply noise capture ▸* and pick a recent capture |
| Track actions | **right-click a track header/lane**: voice-clean the track, remove non-voice, match timbre, rename, remove |
| Add a track | **+ Add Track** |
| Place a clip on a track | drag a clip card up into a track lane (drops at any time offset), or right-click → Add to timeline |
| Move a placed clip | drag it along the track — a live ghost shows where it will land (drag vertically to change tracks) |
| Butt a clip flush against a neighbour | drag it slightly *past* the neighbour's edge — it snaps flush and holds |
| Leave a tiny gap instead | approach the neighbour without crossing it (no pull that way), or pull clear of a snap and come back |
| Change one gap and keep the spacing after it | **`Shift`+drag** a placed clip — every clip after it on that track moves with it |
| Set the space before a clip exactly | right-click it → **Space before this clip…**, type it in seconds |
| Butt a clip against the one before it | right-click it → **Close the space before this clip** |
| Remove a placed clip | right-click it → Remove |
| Set the playhead | click a track lane or the ruler |
| Play all tracks | **▶ Play All** (`Space` when nothing is being previewed) |
| Stop | **■ Stop** |
| Undo / Redo | `Ctrl+Z` / `Ctrl+Shift+Z` (or the toolbar buttons) |
| Add files / Save project | `Ctrl+O` / `Ctrl+S` |
| Open / Save / Export | **File** menu |
| Scroll library | mouse wheel over the clip area |
| Scroll the timeline sideways | drag the **horizontal scrollbar** along the bottom, roll the wheel over the lanes, or `←` / `→` (`Ctrl`+`←`/`→` or `PgUp`/`PgDn` to page) |
| Jump to the start / end of the arrangement | `Home` / `End` |
| Scroll the tracks vertically | `Shift`+wheel, wheel over the track headers, or drag the scrollbar on the right |
| Zoom the timeline | `Ctrl`+wheel over the lanes, or drag the zoom slider |
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
| `dsp.{h,cpp}` | FFT, speech-aware loudness, STFT noise reduction (spectral subtraction / Wiener / Audacity-style profile gate), voice isolation (non-speech removal), LTAS timbre matching, manual silence/delete edits |
| `dialogs.{h,cpp}` | Text prompt, export-options modal, voice-cleaner options, remove-non-voice options, match-timbre options, open/save file & project dialogs |
| `ui.cpp` | Main window: menu bar, layout, painting, hit-testing, all interaction |
| `selftest.cpp` | `--selftest` backend round-trip checks (decode/encode/peaks/DSP) |

All audio is decoded to the project's internal rate (**max(48 kHz, device mix
rate)**) as stereo float, so clips of any source rate/bit depth are unified. When
the device mix rate is below the internal rate, the engine linear-resamples on
the audio thread during playback. Sample-rate / bit-depth / channel / container
conversion for output happens at export time.
