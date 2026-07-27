# Audio Clip Editor — Design

Native-Windows audio clip editor: C++20, Win32 API only (no dialog resources — all
UI is `CreateWindowW` + owner-drawn painting), Media Foundation for decode/encode,
WASAPI for playback. No external audio libraries. Built with CMake + MSVC; the exe
lands in `bin/AudioClipEditor.exe`. `README.md` is the user-facing doc — keep it in
sync with behavior changes.

## Module map

| File | Role |
|---|---|
| `audio_buffer.h` | `AudioBuffer` (interleaved float PCM, `sampleRate`/`channels`/`samples`), `AudioBufferPtr` (`shared_ptr`), `PeakCache` min/max bucket envelope for waveform drawing |
| `model.h` | `Clip` (id, name, buffer, peaks, gain, **timestamp**), `Track` (id, name, gain, placed clips), `PlacedClip` (clipId + start frame), `Project` (library order = display order) |
| `decoder.{h,cpp}` | MF Source Reader → stereo float at the project rate |
| `encoder.{h,cpp}` | WAV writer (manual RIFF; 16/24-bit PCM, 32-bit float) + MF Sink Writer (MP3/AAC/WMA); output-rate resampling |
| `engine.{h,cpp}` | WASAPI shared-mode render thread; `BufferSource` (single-clip preview) and `TimelineSource` (all-tracks mix); linear resample project→device rate on the audio thread |
| `undo.h` | Snapshot-based undo **tree**: every edit stores a full `Project` copy; redo with branch picker |
| `document.{h,cpp}` | Owns `Project` + undo tree + `ViewState`; all mutations go through `commit(desc)`; tracks the last-saved undo node for the unsaved-changes flag (`markSaved`/`isModified`) |
| `dsp.{h,cpp}` | Radix-2 complex FFT, speech-aware loudness, three noise-reduction algorithms + voice isolation (see below) |
| `waveform.{h,cpp}` | GDI oscilloscope: per-column min/max envelope (one `PolyPolyline`) zoomed out, per-sample trace zoomed in |
| `dialogs.{h,cpp}` | Manual modal dialogs: text prompt, export options, voice-cleaner options, file/project pickers |
| `layout.h` | Pure geometry split out of `ui.cpp` so it is headlessly testable: the reflowing library grid's drop targets (`insertIndex`, `caretAnchor`) and toolbar row wrapping (`flowButtons`) |
| `transport.h` | Pure play/pause decision logic, likewise split out to be testable: `decide(State, Press)` → pause / resume / restart-from-where, for both the single combined control and the editor's labelled button pair |
| `ui.cpp` | The whole main window: `App` struct, layout, painting, hit-testing, menus, drag/drop, full-window clip editor |
| `main.cpp` | `wWinMain` → `--selftest` or `runApp()` |
| `selftest.cpp` | Headless `--selftest`: decode/encode round-trips, DSP checks, writes `bin/selftest.log` |

## Core invariants

- **One internal sample rate.** Everything is resampled on load to
  `max(48000, device mix rate)`, stereo interleaved float. DSP and the timeline
  never see mixed rates; export resamples out.
- **Buffers are immutable-by-convention.** Edits produce new `AudioBuffer`s
  (`sliceBuffer`, DSP returns fresh buffers); `Document::commit` snapshots the
  whole project for undo. Never mutate a buffer that a snapshot might share.
- **No dangling pointers into containers.** Clips/tracks are found by id at use
  time (`findClip(id)`), never held as pointers across mutations.
- **UI thread owns everything except the WASAPI render callback**, which only
  reads through the engine's source objects (swapped under a lock).

## DSP: noise reduction (`dsp.{h,cpp}`)

Three algorithms behind one entry point `dsp::denoise(buf, NROptions)`:

1. **SpectralSubtraction** / 2. **Wiener** — automatic: STFT (1024/hop 256),
   noise floor estimated per-bin from the quietest ~10% of frames (assumes speech
   pauses), strength presets Light/Medium/Aggressive.
3. **Profile** (`NRAlgorithm::Profile`) — **faithful reimplementation of
   Audacity's Noise Reduction effect** (verified line-by-line against
   Audacity 3.7.1 `libraries/lib-builtin-effects/NoiseReductionBase.cpp`, in its
   released configuration). Independent clean-room implementation — no GPL code
   copied.

### Profile algorithm details (parity targets)

- STFT: **window 2048, hop 512** (4 steps/window), **periodic Hann** analysis ×
  Hann synthesis (Audacity's WT_HANN_HANN), spectrum bins 0..1024.
- `computeNoiseProfile(buf)`: per-bin **mean noise power** pooled over all
  channels, complete windows only, **no padding**; `windows == 0` (input
  < 2048 frames) ⇒ invalid ("too short").
- Classification (Audacity DM_SECOND_GREATEST): a time-frequency cell is noise
  iff the **second-greatest power among the 5 windows centred on it** ≤
  `sensitivity · ln(10) · profileMean[bin]`. Out-of-range neighbours count as
  zero power.
- Gains: noise ⇒ `10^(−reductionDb/20)`, signal ⇒ 1. Then **attack/release
  smoothing**: backward pass (attack, 0.02 s) and forward pass (release, 0.10 s)
  take `max(g[k], neighbor·oneBlockFactor)` with
  `nBlocks = 1 + (int)(t·rate/hop)`, `oneBlockFactor = 10^(−dB/(20·nBlocks))`.
  (Offline two-pass over full arrays — proven equivalent to Audacity's streaming
  queue.)
- **Frequency smoothing**: geometric mean (mean of logs) of the gain curve over
  `[k−bands, k+bands]`, applied in *both* Reduce and Residue modes.
- **Residue** mode multiplies the spectrum by `(gain − 1)` — phase-flipped — so
  `reduce − residue == original` exactly (selftest asserts maxDiff < 1e-3).
- Reduction pass zero-pads **lead = trail = window − hop** and overlap-adds with
  a `Σwin²` normalizer; output clamped to [−1, 1].
- Options struct `NRProfileOptions` mirrors Audacity's dialog exactly:
  `reductionDb` 0–48 (default **6**), `sensitivity` 0.01–24 (default **6.00**),
  `freqSmoothingBands` 0–12 (default **6**), `residue` bool
  (Noise: Reduce/Residue). (These are the current 3.x factory defaults;
  pre-3.x was 12/6.00/3.)
- `denoiseWithProfile` returns nullptr when the profile is invalid or
  `profile.sampleRate != buf.sampleRate` (Audacity's rate-match rule). Since the
  whole app runs at one internal rate this only fires if a stale profile
  survives a device-rate change.
- Memory shape: pass A stores per-frame **power spectra** only; the gain array
  reuses that storage; pass B recomputes the forward FFT per frame (3 FFTs per
  frame per channel total).

### Profile workflow in the app

Audacity's "Step 1: select noise, Get Noise Profile; Step 2: select audio,
apply" maps onto this app's clip model:

- The **noise selection is the drag-selection on any clip card** (or in the
  full-window editor). Capture via the **Get noise profile** button inside the
  voice-cleaner dialog, or right-click → *Voice cleaner → Capture noise from
  selection* (`IDM_GETPROFILE`, always enabled — shows guidance when there is no
  selection) → `App::captureNoiseProfileFromSelection()`.
- **Capture from a whole clip** (no selection needed): right-click → *Voice
  cleaner → Capture noise from whole clip* (`IDM_GETPROFILE_CLIP`) →
  `App::captureNoiseProfileFromClip(clipId)`, which runs
  `dsp::computeNoiseProfile` over the entire clip buffer (desc `whole clip 'name'
  (s)`).
- **Capture from inside the full-window editor**: the editor toolbar has a
  **Capture noise** button (`EB_CAPTURE`) → `App::captureNoiseProfileFromEditor()`,
  which uses the active editor selection if present, else falls back to the whole
  clip. The button label reflects which (`Capture noise (sel)` / `(clip)`).
- All three capture paths feed the same `rememberCapture()` recents list.
- Reduction applies per the app's scope model (this clip / the selection in this
  clip / this track / all clips) — see *Applying an effect to the selection only*.
  A selection is otherwise just the *noise sample*, not the target.
- **Session persistence**: `App` holds `nrOpts` (last-used options),
  `noiseProfile`/`noiseProfileDesc` (the *active* capture, e.g. `1.20 s from
  'clip'`), and `noiseCaptures` (recent captures, most-recent first). The active
  profile survives across dialog invocations, like Audacity's session profile.
  `NROptions::noiseProfile` (the pointer) is **bound only at call time**
  (`opts.noiseProfile = &noiseProfile;`) — never persisted — to avoid dangling.

### Recent noise captures (reuse without recompute)

- A **`NoiseCapture`** = the computed `dsp::NoiseProfile` + a description. The
  profile (per-bin noise-power means) is the *only* capture-derived precomputation
  that is shared unchanged across every clip a capture cleans — everything else
  (target STFT, classification, gains) depends on the target audio. So the
  captures list caches exactly that profile; re-selecting a capture from recents
  never recomputes it.
- `App::noiseCaptures` (`std::vector<NoiseCapture>`, cap 8, MRU-ordered).
  `rememberCapture()` inserts at the front (skipping a no-op re-add of the current
  front); both `captureNoiseProfileFromSelection()` and the dialog's Get-Noise-
  Profile button (which sets `VoiceCleanerContext::captured`) feed it.
- **Apply from recents**: clip right-click → *Voice cleaner → Apply noise capture
  ▸* lists the captures (`IDM_APPLYCAP_BASE + index`). `applyCaptureToClip()`
  copies the chosen profile into the stable `noiseProfile` member (never holds a
  pointer into the vector across the MRU reorder), forces `NRAlgorithm::Profile`,
  denoises that one clip with the current `nrOpts.profile` options as a single
  undo step, then promotes the capture to the front. Rejects a sample-rate
  mismatch with a message.
- Captures are **session-scoped** (not written to the `.acep` project), matching
  Audacity's session profile behavior.

## DSP: voice isolation — "remove non-voice" (`dsp.{h,cpp}`)

Complementary to noise reduction: NR removes *steady* noise from the whole
signal, voice isolation removes *non-speech events* (bumps, shuffling, door
slams, keyboard, and the room tone between sentences) by gating on speech
detection. Entry point `dsp::isolateVoice(buf, VoiceIsolateOptions, stats*)`;
the frame-level decision is exposed separately as `dsp::detectVoiceFrames` for
tests / future waveform overlays.

- **Analysis**: mono downmix, window 2048 / hop 512 (≈43 ms / ≈11 ms at 48 kHz),
  periodic Hann. Each frame does **one zero-padded FFT (4096)**, from which both
  the band powers *and* — via the inverse FFT of the power spectrum — the linear
  autocorrelation are derived. Two FFTs per frame total; ≈37× realtime.
- **Features per frame**
  - `harmonicity` — largest **interior local maximum** of the autocorrelation over
    F0 lags (70–400 Hz), normalised by `r[0]` and **de-biased by the analysis
    window's own autocorrelation**. Requiring an interior local max is what stops
    a sub-70 Hz thump from faking a peak at the edge of the lag range.
  - `lowFrac` — energy below 150 Hz as a fraction of the total. A bump / desk
    knock / footstep is almost all sub-150 Hz; speech never is.
  - `spFrac` — energy in the 250–4000 Hz speech band as a fraction of the total.
  - `rms` — frame level against a **95th-percentile loud-frame reference**, so
    room tone and distant rustle fall below the gate.
- **Thresholds slide with `sensitivity` (0…1)**: `harmThr = 0.12 + 0.33·s`,
  `lowThr = 0.85 − 0.10·s`, `spThr = 0.06 + 0.10·s`, level factor
  `0.010 + 0.060·s`. The dialog exposes three presets (Gentle 0.25 / Balanced
  0.50 / Strict 0.75).
- **Segment shaping** (this is what makes it usable on real speech): voiced runs
  shorter than **60 ms** are dropped (transients can ring briefly and look
  pitched); surviving runs are grown by a **120 ms pre-roll** and the user's
  **hold** (default 200 ms) — the growth is what preserves unvoiced consonants
  (`s`, `f`, `t`) that flank voiced speech; then gaps shorter than **150 ms** are
  merged so words aren't chopped mid-utterance.
- **Bump override (applied last, beats the growth)**: a frame that is audible,
  **≥ 90% sub-150 Hz** and carries **≤ half the speech-band threshold** is marked
  as a definite non-voice *event* and forced out of the mask after the growth and
  gap-merging have run. Runs shorter than 40 ms are ignored so a single frame
  can't punch a hole through a word. The override only ever touches frames the
  detector already rejected, so it cannot take a bite out of detected speech.
  Without it the pre-roll / hold / gap-merge protected any thump landing within
  ~120–200 ms of a sentence — the normal place for a bumped desk or a door slam,
  and precisely what a user selects when they want one gone. Such a bump came
  back at **−0.9 dB** while an identical isolated one went to **−60 dB**, which
  made the effect look broken on selections and made a whole-clip run look as
  though it had skipped the selected region. It is an *identification* test, not
  a strength knob, so it is deliberately not tied to `sensitivity`.
- **Application**: the frame mask becomes sample segments, then the output is
  written in one pass with a moving segment index — gain 1 inside a segment,
  `10^(−reductionDb/20)` outside, with a raised-cosine ramp of `fadeMs` on each
  side (no per-sample envelope array, so a long clip costs no extra memory).
  `residue` outputs `1 − gain`, so **reduce + residue == original exactly**
  (selftest asserts maxDiff < 1e-5).
- **Length is never changed** — non-voice is attenuated in place, never cut, so
  timeline placements and selections stay valid. `VoiceIsolateStats` reports
  kept/removed seconds and the segment count for the confirmation message.
- Audio shorter than one window (2048 frames) is treated as all-voice rather than
  failing, so the call never returns nullptr for non-empty input.

### What the detector cannot do (and why the manual edits exist)

The bump override above catches *low-frequency* thumps. There is a second class of
non-speech event it deliberately does not attempt: sounds that are **harmonic,
mid-band and syllable-length** — chair creaks, swallows, a shuffle that happens to
ring. Measured on a real user recording (`bumps.wav`, three events in 0.56 s) these
score `harm` 0.49–0.98, `lowFrac` ≈ 0, `spFrac` 0.46–0.98: every one of the four
core voice tests passes, at **every** sensitivity from 0 to 1, so no setting removes
them. Two candidate discriminators were measured against the same speaker's real
takes and both were rejected:

| Candidate | Result |
| --- | --- |
| Presence of energy above 4 kHz | Refuted — this speaker's *speech* also has 0.0–0.4% above 4 kHz |
| 400–1000 Hz vs 150–400 Hz band balance | Refuted — 15–46% of real speech frames land on the "bump" side |

The conclusion is that these events are not separable from voiced speech by any
per-frame spectral feature. The proper answer is therefore not a cleverer detector
but a **manual route** — `dsp::silenceRange` / `dsp::deleteRange` below — for the
case where the user can hear the problem and the machine cannot.

### Manual region edits

Two operations on the waveform selection, both in `dsp.cpp` so the selftest can
exercise them headlessly, both committed through `Document::replaceClipBuffer` as
one undo step by `App::removeSelectedRange(clipId, keepLength)`:

- **`dsp::silenceRange(src, begin, end, fadeMs = 5)`** — zeroes `[begin, end)`
  with raised-cosine ramps **inside** the range at each edge, so audio outside the
  selection is bit-identical and the clip's length is unchanged. Ramps are capped
  at half the range so a very short selection still reaches zero. This is the one
  to use between sentences: the pause stays the length it was, so nothing
  downstream on the timeline shifts.
- **`dsp::deleteRange(src, begin, end, fadeMs = 5)`** — cuts `[begin, end)` and
  closes the gap with an **equal-power** (sin/cos) crossfade. The overlap is taken
  from the audio being *kept* on either side — never from the removed material,
  which would defeat the point — so the result is `fadeMs` shorter than a plain
  concatenation, and the crossfade shrinks to nothing when the cut touches the
  clip's start or end. Equal-power rather than linear because the two sides are
  unrelated room tone; a linear fade would dip audibly there.

Both return `nullptr` for an empty range, and `deleteRange` also refuses to delete
the entire clip (that is what *Delete clip* is for). Silencing keeps the selection
alive so the result can be auditioned with *Play selection*; deleting clears it,
because everything after the cut has moved.

Reachable from the clip context menu (`IDM_SILENCESEL` / `IDM_DELETESEL`, next to
*Crop to selection*, which is their inverse) and from the full-window editor
toolbar (`EB_SILENCE` / `EB_DELSEL`).

Adding those two buttons pushed the editor toolbar past the window width, so
`computeEditorLayout` now **wraps** it via `layout::flowButtons`: buttons flow
left-to-right and start a new row when the next would collide with the *Done*
button's reserved strip, and the resulting height is published as
`App::edToolbarH` for the painter and the content area below (floored at the
historical `S(56)` so an unwrapped toolbar is laid out exactly as before). The row
already didn't fit at the default 1500 px window above 125% DPI, where the
rightmost buttons ran off the edge and were simply unreachable — that is a
pre-existing bug this fixes. Keeping the flow in `layout.h` means the selftest can
assert the wrapping at both 100% and 150% DPI without a window.

### Voice isolation in the app

`App::viOpts` (session-persisted options) → `removeNonVoiceClip` /
`removeNonVoiceClipSelection` / `removeNonVoiceTrack` / `removeNonVoiceAllClips`
funnel into `removeNonVoiceClips(ids, scopeLabel, selectionOnly)`, mirroring the
voice cleaner's scope model: one options dialog (`dlg::voiceIsolate`), then every
target clip is processed and committed through `Document::replaceClipBuffers` as
**one undo step**. Menu ids `IDM_VOICEISO` / `IDM_VOICEISO_SEL` /
`IDM_VOICEISO_ALL` (clip menu submenu *Remove non-voice*) and `IDM_TRK_VOICEISO`
(track header menu). A summary message box reports how much was silenced across
how many voice segments — counted over the processed range only, so a
selection-scoped run cannot quote whole-clip numbers.

## Applying an effect to the selection only

Both the voice cleaner and remove-non-voice can be limited to the current
waveform selection instead of the whole clip.

**The range is chosen in the right-click menu, not in the dialog.** Each effect's
submenu lists its scopes directly:

```
Voice cleaner (reduce noise)  ▸  This clip (selection)…
                                 This clip…
                                 All clips (whole project)…
                                 ─────
                                 Capture noise from selection
                                 …
Remove non-voice (…)          ▸  This clip (selection)…
                                 This clip…
                                 All clips (whole project)…
```

This puts the *what* in front of the user before they commit to opening a dialog,
so "only the selection" is discoverable without having to open one and hope. The
`(selection)` item is **greyed, not hidden**, when nothing is selected on that
clip — same reason. The dialogs then only choose the *how*.

The pieces:

- **`App::selectionCovers(clipId)`** (`ui.cpp`) — gates the `(selection)` items:
  there must be a selection, it must belong to *that* clip, and it must be
  non-empty. A selection left on some *other* clip can never narrow an operation.
- **`App::selectionScopeLabel(clipId)`** (`ui.cpp`) — builds
  `the selection in 'take 2' (0:12.30 – 0:18.00, 5.70 s)`.
- **`VoiceCleanerContext::scopeLabel` / `VoiceIsolateContext::scopeLabel`** — the
  caller-built string both dialogs restate on their **"Applies to:"** line, so the
  menu choice is visible where the settings are chosen.
- **`ScopeLine`** (`dialogs.cpp`) — lays that line out. It is normally the longest
  text in either dialog, so it may widen the frame (up to `ui.S(460)`), then wrap
  onto a second line, and only then is ellipsised. `widen()` must be called before
  anything else is measured against `contentW`.
- **`voiceCleanClips(ids, scopeLabel, selectionOnly)` /
  `removeNonVoiceClips(ids, scopeLabel, selectionOnly)`** — `selectionOnly` is
  re-validated inside (single target + `selectionCovers`) rather than trusted.
- **`dsp::blendProcessedRange(original, processed, begin, end, blendMs)`** — does
  the actual restriction. Returns a copy of `original` with `[begin, end)` taken
  from `processed`, raised-cosine crossfaded over ~5 ms at each edge (capped at a
  third of the range) so the join can't click.

Track-header and all-clips scopes are inherently multi-clip, so they have no
selection variant.

**The effect always runs over the whole clip and only the result is narrowed** —
never "slice the selection out, process the slice, paste it back". This is the
important design point: the auto NR algorithms estimate their noise floor from
the quietest frames of whatever they're given, and voice detection needs the
surrounding context, so a short slice analysed in isolation would produce a
different and usually much worse result. Processing whole and keeping part means
the audio inside the selection is exactly what a whole-clip run would have put
there. The cost is that a two-second selection still pays for a whole-clip pass,
which at ≈50× realtime is not worth optimising.

`dsp::isolateVoice` takes optional `statsBegin`/`statsEnd` so its
`VoiceIsolateStats` cover only the kept range — otherwise the confirmation
message would quote whole-clip numbers for a selection-sized edit. The undo
label and the summary message both say "the selection" / "(within the
selection)" when the run was narrowed.

## Timeline: moving a placed clip (drag feedback)

`Mode::ClipMove` (started in `onLDown` when `placedAt(p)` hits) drags a placed
clip. During the drag `paintTimeline` draws a **live ghost** of the clip (name +
waveform) at the snapped target position under the cursor — coloured
`clipBlkSel` when the drop is legal, `stop` (red) when it would overlap — while
the clip's home slot is left as a faint outline. The actual `Document::moveClip`
happens on `onLUp`. The ghost mirrors the existing `Mode::CardDrag`
(library-card → track) drop preview and uses the same `snapFrame` /
`Track::overlaps(start, len, ignore)` logic as the commit, so what you see is
where it lands.

## Library ordering (reorder + sort)

The clip library's display order **is** `Project::library`'s vector order
(`layoutCards` walks it in order), so reordering means reordering the vector.

- **Drag-reorder**: dragging a card by its title bar starts `Mode::CardDrag` (the
  same gesture as drag-to-track). Dropping inside `rcLibrary` (rather than on a
  track) calls `Document::moveClipInLibrary(clipId, libInsertIndex(p))`.
  `moveClipInLibrary` erases then re-inserts, adjusting the target for the removal,
  and is a no-op (no undo step) when the drop wouldn't change position.
  `paintLibrary` draws a live accent **insertion caret** at the drop point and
  outlines the dragged card in accent; the cursor shows `IDC_SIZEALL` over a drag
  handle / during a card drag.

  The drop geometry itself lives in **`layout.h`** as pure functions of the
  laid-out card rectangles, so it is regression-tested headlessly rather than only
  by eye:

  - `layout::insertIndex(cards, p)` — the reading-order index (a card precedes the
    cursor if it's on an earlier row, or on the same row with its centre left of
    the cursor; below every row appends).
  - `layout::caretAnchor(cards, p, idx)` — which card the caret hangs off and on
    which side. **A row boundary is one index but two places**: the empty tail of
    one row and the head of the next both mean "insert at *i*". The caret used to
    be drawn unconditionally at `cards[idx].left`, so aiming at the end of a row
    made it jump down to the head of the next one and end-of-row drops looked
    impossible. `caretAnchor` returns `{card, trailing}` and picks the *place* the
    cursor is actually in, so the caret stays under the pointer.

  Note the reflow consequence, which is inherent and not a bug: a clip dropped at
  the tail of a full row lands at that index and therefore *renders* at the head of
  the next row. The order is exactly what the caret promised.
- **Sort**: right-clicking the empty library area (`onRDown` → `libraryContextMenu`)
  offers *Sort by name (A–Z)* and *Sort by time (oldest first)* →
  `Document::sortLibrary(byName)`. Name sort is case-insensitive (`_wcsicmp`) with
  an id tiebreak; time sort uses `Clip::timestamp`. Both `stable_sort` and only
  commit an undo step if the order actually changed.
- **`Clip::timestamp`** (FILETIME ticks) is set in `Document::addClip` from the
  source file's last-write time, or the current time for derived clips (crop /
  save-selection, no source path). It is persisted in the `.acep` file (**format
  v2**); loading a v1 project falls back to the source file's current mtime so
  time-sort still works.

## Getting audio back out (export)

Edits are **non-destructive to the source file**. `Document::replaceClipBuffer(s)`
only swaps the in-memory buffer, rebuilds peaks and re-syncs placement lengths;
`Clip::sourcePath` is never opened for writing (it is read only for the
sort-by-time mtime). The edited audio therefore lives solely in the `.acep`,
which embeds raw float samples — so the only ways out are:

- **`App::exportMix()`** — *File → Export Mixdown*: `Document::renderMix()` of all
  tracks, defaults to the project rate and stereo.
- **`App::exportClipAudio(clipId, selectionOnly)`** — clip menu *Export clip to
  file…* / *Export selection to file…* (`IDM_EXPORTCLIP` / `IDM_EXPORTSEL`, the
  latter greyed via the same `sel` flag as the other selection entries). Uses
  `sliceBuffer` for the selection case and defaults to the **clip's own** rate and
  channel count, so a plain export neither resamples nor upmixes a mono take.

Both go through `dlg::exportOptions` → `mfio::encodeFile`. The suggested file name
passes through **`mfio::safeFileName`** because a clip name is free text but is
dropped straight into `GetSaveFileNameW`'s buffer: path-illegal characters become
`_`, and leading/trailing spaces and trailing dots are stripped (Windows discards
those silently, so the file would land under a different name than the dialog
showed). A name that sanitises away to nothing falls back to `clip`.

Neither export touches the project: exporting is not a save, and does not clear
the unsaved-changes flag.

## Project view state (selection + playhead persistence)

The waveform selection (`selClipId`/`selStart`/`selEnd`) and the playhead are
*UI* state and live in `App`, but they're saved with the project so reopening
puts you back where you left off. `Document` holds a `ViewState` struct
alongside — deliberately **not inside** — `Project`:

- Being outside `Project` keeps it out of the undo snapshots, so dragging a
  selection neither creates an undo step nor gets rewritten by undo/redo.
- It **does** feed `isModified()`. `markSaved()` snapshots the selection along
  with the undo node, and `isModified()` compares `selClipId/selStart/selEnd`
  against it — a selection is deliberate work, so losing one to an unprompted
  exit would be worse than an extra save prompt. The **playhead is excluded from
  the comparison** (though still saved): `onTimer` reassigns it from
  `engine.position()` during timeline playback, so counting it would mark the
  project dirty just for pressing Play.
- `App::storeViewState()` copies the live selection/playhead into `doc.view()`;
  `App::restoreViewState()` reads it back after a load. Because the dirty flag
  now depends on the selection, the UI must never call `doc.isModified()`
  directly — `App::projectModified()` is the single choke point that stores the
  view state first, and the title bar, the timer's `*` refresh and
  `confirmDiscardChanges()` all go through it.
- `.acep` **format v3** appends the block (`selClipId`, `selStart`, `selEnd`,
  `playheadFrame`) **after the tracks**, so the older sections parse identically;
  v1/v2 files simply load with a default (empty) view state.

### Surviving edits: `clampSelection`

A selection is positioned by hand, so it is only ever **narrowed**, never thrown
away as a blanket precaution. `clampSelection(project, clipId, start, end)`
(`document.{h,cpp}`) is the single rule: clamp the bounds to the clip's current
length, and drop the selection (`clipId = -1`, bounds zeroed) only when nothing
of it survives — the clip is gone, or the range no longer overlaps any audio.

Both places that can invalidate a selection go through it, so the two cannot
drift apart:

- `Document::loadProject` validates on the way in — a selection whose clip is
  gone, or whose bounds fall outside the clip (e.g. it was cropped in another
  session), is dropped rather than restored as a bogus highlight.
- `App::validateSelection()`, called from `App::afterHistory()` — the common tail
  of undo, redo, removing a clip, removing a track and removing a clip's
  *placement* from a track. This used to clear the selection outright, which is
  the bug where removing a clip from the timeline silently wiped the selection on
  the corresponding **library** clip: that edit doesn't touch the clip's audio at
  all, so there was nothing to invalidate.

Validating (rather than restoring from `ViewState`) is the right move after
undo/redo precisely because `ViewState` sits outside the undo tree — there is no
older selection to roll back to, only the live one to check.

## Clip preview playback (cursor, play/pause, seeking)

Clip auditioning is driven from `App` by a small block of preview state, with
`previewCursor` as the **single authoritative play position** (in clip frames):
it is what the yellow cursor is drawn at, and what play/resume starts from.

- `previewClipId` — the clip the armed source belongs to (`-1` = none).
- `previewBegin` / `previewEnd` — the frame range the armed `BufferSource`
  covers: the whole clip, or `[selStart, selEnd)` for a selection audition.
  `previewIsSel` records which of the two it is.
- `previewSeekPending` — the cursor was moved while stopped/paused, so the next
  play must re-arm the source instead of resuming the engine in place.

Three functions own all of it:

- **`startPreview(clipId, startFrame, useSel)`** arms a `BufferSource` over the
  chosen range and starts it at `startFrame - previewBegin` (the source's
  `seek`/`position` are begin-relative). A start frame outside the range snaps to
  the range start, which is what makes "play again after it finished" restart.
- **`togglePreview(clipId, useSel, ownRangeOnly)`** is the play/pause button for
  both the library card (`togglePlayClip` → `useSel = true`, so the button
  auditions the selection when there is one) and the clip editor (`EB_PLAY` →
  whole clip, `EB_PLAYSEL` → selection). It is a thin translation layer: the
  decision is `transport::decide` (see below), and this just applies the result
  to the engine.
- **`seekClip(clipId, frame)`** moves the cursor. While playing it re-arms
  immediately (staying inside the selection audition only while the target is
  still within it); while stopped or paused it just records `previewCursor` and
  sets `previewSeekPending`, so clicking the waveform decides where the next
  play/resume picks up.

`onTimer` tracks the cursor as `previewBegin + engine.position()` and `onPlayEnd`
parks it at `previewEnd` — neither reads the live selection, so editing the
selection mid-playback can't drag the cursor around. `stopAll()` clears the whole
block. The selftest covers the `BufferSource` sub-range arithmetic these rely on
(begin-relative seek/position and stopping at the range end).

### Which control does what — `transport::decide` (`transport.h`)

A clip can be auditioned over two ranges (whole clip / selection) and there are
two *kinds* of control, which is what makes the rule subtle enough to be worth
isolating. `transport.h` is pure logic — no Win32, no engine — so the whole truth
table is regression-tested headlessly. `decide(State, Press)` returns a `Plan`:
`Act::Pause | Resume | Restart`, plus `From::Cursor | SelStart | ClipStart` for
where a restart begins.

- **`ownRangeOnly = false` — a single combined control** (the library card's
  play/pause button, the Space key). It stands for *whatever* is playing, so it
  pauses any running preview of the clip. Without this its pause button would
  restart a selection audition instead of pausing it.
- **`ownRangeOnly = true` — one of a labelled pair** (the editor's **Play** and
  **Play selection**). Each speaks for one range: it pauses only what *it*
  started, and pressing it while the *other* range is playing switches playback
  to itself. The editor's two buttons therefore each show `❚❚ Pause` / `❚❚ Pause
  selection` only while their own range runs.

  This is the fix for a reported bug: both buttons used to share the combined
  rule, so pressing **Play selection** flipped the neighbouring **Play** button to
  Pause — a button the user had not pressed appeared to react. The paint code
  matches: `playingSel`/`playingAll` are derived from `previewIsSel`, and the
  accent colour now marks *a transport that can be started*, so **Play selection**
  is accented whenever a selection exists rather than being permanently grey while
  **Play** was permanently green.

- **Resume happens in place only when the armed source still matches** what was
  asked for (same clip, same range) and no seek is pending — otherwise it re-arms.
  A moved selection edge counts as a different range and re-arms at the new head.

`EB_PLAYSEL`'s width in `computeEditorLayout` is sized for its **widest**
alternate (`Pause selection`), because resizing a button the instant playback
starts would re-flow the whole toolbar under the cursor. Since labels and widths
are declared in different places and `button()` draws with `DT_CENTER` and no
ellipsis (a too-long label is silently clipped at both ends), the selftest
measures every toolbar label — including every alternate a button swaps to — with
the real Segoe UI font at 100% and 150% and asserts each fits with ≥6 px spare.

## Playhead accuracy and smoothness

The play cursor has to answer "which sample am I hearing *right now*", and three
separate things stood between the naive implementation and that answer. All three
had to be fixed for the cursor to look continuous; each on its own left it jumpy.

**1. Heard position, not render position (`engine.{h,cpp}`).**
`IPlaybackSource::position()` is where the *mixer* has read to, which leads what
is audible by the whole WASAPI buffer depth (30 ms here) and advances in
chunk-sized lumps rather than continuously. `RenderTimeline` closes that gap: the
audio thread records, for each chunk it renders, how many device frames had been
written and which source frame that chunk began at, and the UI thread converts
"device frames the hardware has actually played" back into a source frame,
interpolating **within** a chunk so the answer moves continuously. Device frames
played come from `IAudioClock::GetPosition(&pos, &qpcPos)` — a stream position
plus a QPC timestamp in 100 ns units — extrapolated to "now" with
`QueryPerformanceCounter`. The clock is sampled **on the audio thread** and the
result published for the UI thread, so the UI never touches the COM object.
`position()` finally returns `max(0, min(heard.sourceAt(devPlayed), rendered))`,
clamped so it can never run past what has been produced. Measured effect: the
per-timer-tick advance rate went from swinging 0.634x–1.314x of real time to
0.9991x–1.0008x.

**2. The timer really fires at ~31 ms when you ask for 16 (`ui.cpp`).**
`setTimerRate(playing)` switches `SetTimer(hwnd, 1, …)` between a fast rate while
something plays and 33 ms when idle, since the playhead is the only thing in the
window that animates. The fast rate is **15 ms, not 16**: `WM_TIMER` fires on the
system clock tick (~15.6 ms by default) and a requested period is rounded **up**
to a whole number of ticks, so 16 ms waits two ticks. (Measured: with 16 the
playhead stepped every 33–35 ms — no better than idle.) 15 ms is also safe if
something else on the machine has raised the timer resolution: it caps the
repaint rate at ~66 Hz rather than running away.

**3. A repaint cost 29 ms, so the UI ran at 34 fps flat out (`waveform.cpp`).**
This was the dominant cause and it hid the other two. The envelope path used to be
a single `Polygon` tracing the max edge left-to-right and the min edge back again;
GDI has to scan-convert that ~3000-vertex, wildly self-overlapping outline,
intersecting every edge with every scanline. Profiling attributed **11.5 ms** to
the main `wf::draw` and **11.0 ms** to the selection-coloured overlay draw — the
editor issues both every paint. It is now one **`PolyPolyline` of per-column
two-point vertical segments**, which draws the identical picture: ~1.0 ms for the
main draw, 5.4 ms median for the whole paint, and the repaint interval settles at
the intended 15.6 ms median (64 Hz). Because each column is stroked independently
it also cannot produce the tangles the polygon did where the two edges crossed.
Two details matter: `Polyline` does not draw its final point, so a silent column
is forced to `yBot = yTop + 1` rather than vanishing; and the columns come from
the raw samples when `framesPerPx < pc.bucketFrames` (else the cache would look
blocky) and from `PeakCache::rangeMinMax` otherwise.

`paint()` was already double-buffered (`CreateCompatibleDC` + `CreateCompatibleBitmap`
+ `BitBlt`, with `WM_ERASEBKGND` returning 1), so no tearing work was needed.

**The audio driver's clock is not linear for the first ~130 ms.** After
`IAudioClient::Start()`, `IAudioClock` reports exactly one **~12 ms (510–530
frame) downward step**, 60–130 ms in, and thereafter tracks QPC to within 0.1%
for the rest of the stream. It is a property of the *device clock reading
itself* — verified with `PlaybackEngine::posDiag()`, which exposes `devPlayed`
alongside the mapped `heard`/`rendered` values and both clamp flags: at the step,
`devPlayed` dips by the full amount while neither clamp is engaged and the
render lead stays healthy. So the engine has nothing to correct; it is faithfully
reporting what the hardware says, and the step is a one-off that never
accumulates.

This mattered because the selftest's stall check used to skip a fixed **tick
count** (`i < 4`) to clear startup, which made it fail about **one run in six**
at 0.60–0.65×. The transient happens at a fixed time after `Start()`, but a tick
count anchors to whenever the sampling loop began, so the step fell inside the
skip on most runs and just outside it on the rest. The check now skips by
**elapsed time since the stream started** (350 ms), which is the quantity the
transient is actually tied to. Two measurements pinned the cause down and are
worth keeping in mind for anything similar: raising the process priority made the
failure *more* frequent (5/8 rather than 1/8), which rules out CPU starvation
since starvation would go the other way; and the per-tick `dt` stays a uniform
~31 ms straight through the dip, so there is no scheduling gap. `posDiag()` is
retained, and the check dumps the full per-tick series (dt, Δpos, rate,
`devPlayed`, `heard`, `rendered`, clamp flags) whenever it fails — the playhead
maps three independent clocks onto each other, and the reported frame number
alone cannot say which one misbehaved.

**Playhead in the fine-tune strips.** `paintFineStrip` draws the cursor whenever
it falls inside that strip's window:
`if (previewClipId == editClipId && previewCursor >= ws && previewCursor < we)`.
The strips are where accuracy is most visible — at their zoom (a few hundred ms
across half the window) a 33 ms step is over a hundred pixels, which reads as a
stutter however accurate the position underneath it is. End-to-end measurement
after all three fixes: the cursor advances 36–37 px per 16.7 ms sample, i.e.
15.3–15.7 ms of audio per screen refresh, uniformly (it was 66–70 px lumps at
irregular ~33 ms intervals).

## Where the selection is drawn

**A selection belongs to the clip, not to the view that made it.** There is one
selection in the whole app (`selClipId` + `selStart`/`selEnd`), so *every* surface
that shows that clip's audio has to show it — otherwise the same audio appears in
two places looking like two unrelated pieces, which is exactly the bug that was
reported: a selection dragged on a library card was invisible on that clip's
block in the track lane.

`drawSelOverlay(hdc, wv, clip)` is the single implementation, shared by the
library card and the timeline placement (the editor's main view and fine-tune
strips have their own zoomed mapping). It takes a rect across which the clip's
**whole** buffer is spread and paints the tinted band, the waveform redrawn in
`col::waveSel` inside it, and an edge line each side. Two details:

- The frame→x mapping deliberately uses the full, untrimmed `wv`, because a
  placed clip can extend past the visible lane; callers clip with
  `IntersectClipRect` so the geometry still agrees with the `wf::draw` beneath.
- A sub-pixel selection is widened to one pixel, so a very short selection on a
  narrow placement is still visible rather than collapsing to nothing.

Callers guard with the existing `selectionCovers(clipId)` rather than re-deriving
"does this clip own the selection".

Verified by measurement rather than by eye: with the timeline zoomed so the
placement is ~158 px wide, the band matches the card's to **0.63 px** at the start
edge and **0.46 px** at the end — pure rounding. (At the default scale a 0.4 s
placement is only ~26 px, where one pixel is 4% of the block and nothing finer
than that can be concluded.)

## Selection editing (independent edges)

A selection can be adjusted one edge at a time instead of redrawn. All three
selection surfaces use the same anchor-based sweep: the grabbed edge follows the
cursor while the opposite edge is pinned as the anchor, and `selStart/selEnd` are
kept sorted via `min/max` so the highlight never blinks or needs a swap on mouse-up.

- **Card waveform** (`Mode::WaveSelect`): on `onLDown`, if a selection already
  exists on the clicked clip, `hitSelEdge` (grab band `selEdgeGrab() == S(8)` px
  each side, nearer edge wins) decides whether the press grabbed the start edge,
  the end edge, or neither. An edge grab pins the opposite edge in `waveAnchor` and
  sets `waveEdgeDrag`; otherwise a fresh selection starts at the click. A no-move
  edge grab leaves the selection intact (only a non-edge plain click seeks/clears).
- **Editor main view** (`editDrag == 1`): same logic against `edMainX(selStart/End)`,
  pinning `mainDragAnchor` and setting `mainEdgeDrag`.
- **Editor fine-tune strips** (`editDrag == 2/3`): each strip is a dedicated edge
  control — the whole lane is the grab zone (`beginStripDrag`/`updateStripEdge`).
- **Cursor hint**: `WM_SETCURSOR` shows `IDC_SIZEWE` when `overSelEdge(p)` (hover
  near an edge on any surface) or `draggingSelEdge()` (an edge drag in progress).

**Esc cancels a drag in progress** (`cancelDrag`). Every one of these gestures
mutates `selStart`/`selEnd` live as the cursor moves, so abandoning one has to
*restore* the previous selection, not merely stop tracking. `onLDown` therefore
snapshots the selection into `selSaveClipId`/`selSaveStart`/`selSaveEnd` on
**every** button-press, not only on the presses that start a selection drag: a
press cannot yet tell what it will become — a card sweep turns into a
drag-to-timeline the moment the pointer leaves the library, and that conversion
clears the selection. `Mode::CardDrag` and `Mode::ClipMove` are cancellable for
the same reason (and so a mis-aimed drag drops nothing); the continuous drags —
volume, zoom, the scrollbar — are not, because they show their value moving under
the cursor and have no half-finished result to abandon.

`cancelDrag` returns whether it cancelled anything, which is what lets Esc keep
both meanings in the editor: mid-drag it abandons the gesture, and only otherwise
does it close the editor. It clears the drag state, releases capture and restores
the snapshot; the mouse-up that eventually arrives finds `mode == None` /
`editDrag == 0` and so does nothing.

## Dialog layout scaffolding — `DlgUI` (`dialogs.cpp`)

Every dialog in the app is built in code (no `.rc` resources), so nothing lays
itself out automatically. `DlgUI` is the shared helper that makes those dialogs
**DPI-aware and self-sizing**; all four dialogs (`promptText`, `exportOptions`,
`voiceCleaner`, `voiceIsolate`) go through it and none of them contain hardcoded
pixel coordinates.

Construction (`DlgUI ui(parent)`) captures the parent's DPI via
`GetDpiForWindow`, builds the matching UI font with
`SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, …, dpi)` (owning it and
deleting it in the destructor; falls back to `DEFAULT_GUI_FONT`), measures the
line height, and scales the base metrics `margin`/`gap`/`rowGap`.

- `S(v)` — `MulDiv(v, dpi, 96)`; the only way a literal becomes a pixel value.
- `measure(text, wrapWidth)` — `DT_CALCRECT | DT_NOPREFIX` (plus `DT_WORDBREAK`
  when a wrap width is given) on a screen DC with the dialog font selected.
  Everything else is derived from it: `textW`, `btnW`, `checkW`, `comboW`
  (adds room for the drop-down arrow), `rowH`, `editH`, `btnH`.
- `create(cls, title, parent, clientW, clientH)` takes a **client** size and
  converts it with `AdjustWindowRectExForDpi`, then clamps the result into the
  parent monitor's work area. (Passing the size straight to `CreateWindowExW`
  was the original clipping bug — it treats those as *window* extents, so the
  frame and caption ate into the content.)
- `ctl/label/rowLabel/edit/combo/button/check/radio` create children with the
  dialog font already applied; `rowLabel` vertically centres its text against
  the control on the same row and uses `SS_ENDELLIPSIS` so a long label degrades
  gracefully instead of overflowing.

The layout convention in each dialog: measure the widest label and the widest
control string, derive `labelW`/`ctlW`/`contentW`, walk a running `y` down the
rows, then compute the client height from that `y` and right-align the buttons
at `contentW`. Intro paragraphs are stored as one flowing string and wrapped by
`measure(text, contentW)`, so they can never clip mid-line.

**Checking the layout**: `AudioClipEditor.exe --dialogtest N` (`main.cpp`) brings
up one dialog over a dummy parent so it can be eyeballed / screenshotted at any
DPI without loading a project — 1 = Remove Non-Voice, 2 = Voice Cleaner,
3 = Save As, anything else = the text prompt. Append `sel` (e.g.
`--dialogtest1sel`) for a selection-scoped "Applies to:" label, which is the
widest thing either effect dialog has to fit (see `ScopeLine`).
Because these dialogs size themselves from measured text, this is the only way to
catch a layout regression short of running the real workflow.

Voice Cleaner additionally **resizes per mode**: `VCState` caches `clientW`,
`autoH`/`profH` and `autoBtnY`/`profBtnY`, and `vcUpdateVisibility(st, dlg)`
moves Apply/Cancel and resizes the frame (again via `AdjustWindowRectExForDpi`)
when the algorithm combo switches between the auto algorithms and Profile — so
auto mode has no empty gap where the profile rows would be.

## Voice-cleaner / remove-non-voice dialogs (`dialogs.cpp`)

Manual modal (no resource script), same pattern as the export dialog:
`VoiceCleanerContext` carries `opts`/`profile`/`profileDesc` in/out plus the
current `noiseSelection` slice. Algorithm combo (Spectral subtraction / Wiener
filter / Noise profile (Audacity-style)) drives row visibility: auto algorithms
show the Strength combo; Profile shows Get-profile button + status line +
dB/Sensitivity/Bands edits + Reduce/Residue radios. Apply is blocked (message
box) when Profile is chosen without a captured profile. Numeric edits parse with
`wcstod`, clamp to Audacity's ranges, and fall back to defaults on garbage.

`dlg::voiceIsolate(parent, VoiceIsolateOptions&, scopeLabel)` follows the same
pattern: a Sensitivity preset combo, numeric Attenuation / Hold / Fade edits
(same `vcReadDouble` clamp-and-fallback parsing) and Keep-voice / Preview-removed
radios, with the scope (`'clip name'`, `all clips (N clips)`, `track 'x'`) shown
in the dialog so a project-wide run can't be triggered by accident.

## Selftest

`--selftest` (headless, logs to `bin/selftest.log`). Profile-NR coverage:
capture validity + too-short rejection, 20 dB reduction attenuates a noise-only
region by ≈20 dB (measured −20.00 dB) while preserving ≥70% of an embedded tone
(measured 99.5%), reduce−residue==original identity, rate-mismatch rejection,
dispatch through `denoise()` incl. missing-profile rejection. Also covers a
`Document` `.acep` **v3 round-trip** (library order, per-clip timestamps, and the
saved selection + playhead preserved; a stale selection is dropped on load),
library **sort-by-name / sort-by-time / reorder**, the **library drop geometry**
in `layout.h` (insertion index for each region of a reflowed 3-per-row grid, and
the caret anchor for each — including the regression where a row's empty tail and
the next row's head share an index but must draw different carets), the **editor
toolbar wrapping** also in `layout.h` (one row when it fits with the height
unchanged, two rows at 150% DPI on the default window, no button ever past the
window edge, wrapped rows restarting at the left margin, and every button still
placed at an absurd 300 px width),
**`clampSelection`** (a selection survives removing an unrelated placement or
deleting a different clip — the reported bug — is clamped rather than dropped
when it merely runs past a shortened buffer, and is dropped only when its clip is
gone or it lies entirely past the end), the unsaved-changes flag
(an edit or a selection change dirties the project, saving clears it, moving the
playhead does not dirty it), and the `BufferSource` sub-range behaviour the clip
preview depends on (span, begin-relative seek/position, rendering from the seek
point, stopping and draining at the range end).

**The preview transport** (`transport::decide`) is covered as a truth table: each
editor button pauses only its own range and *switches* playback rather than
pausing when the other range is running (the reported bug), the single combined
control still pauses either range, pressing the same button again resumes in
place instead of jumping to the range start, a pending seek forces a re-arm, a
moved selection edge re-arms at the new head, and an unrelated clip / timeline
playback start fresh.

**Waveform rendering is tested off-screen.** `wf::draw` is GDI, but it draws into
whatever DC it is given, so a `CreateDIBSection` (32bpp `BI_RGB`, negative
`biHeight` for top-down) makes it fully headless: render, `GdiFlush()`, then read
the pixels back as `DWORD` BGRA and count coloured pixels per column. On a buffer
whose left half is a 300 Hz tone and right half is silence the checks are: a loud
column fills most of the height, a **silent column still collapses to a 1-px
centre line** rather than vanishing, **every** pixel column is drawn, the
raw-sample zoom (`10 frames/px`, past the bucket size) still draws an envelope,
the sub-sample zoom (`4 px/frame`) draws a scope trace, and an empty/inverted
range draws nothing rather than asserting. This exists because the envelope path
was rewritten from a `Polygon` to a `PolyPolyline` purely for speed, and the
correctness of that rewrite is entirely about the column geometry.

`mfio::safeFileName` is covered directly (clean names untouched, path-illegal
characters substituted, leading/trailing blanks and trailing dots stripped, and
the fallback for a name that sanitises away to nothing) — it lives in `encoder.h`
rather than `ui.cpp` precisely so it can be.

Voice isolation is checked against a synthetic take with a bump **150 ms after
the speech** as well as an isolated one: both must be removed (measured −21.0 dB
and −60.0 dB — the adjacent one cannot quite reach the isolated one's figure
because its energy sits in its first few ms and the gate still needs `fadeMs` to
close) while the neighbouring speech is untouched (0.00 dB). That is the
regression the bump override exists for.

The manual region edits are checked on a steady 220 Hz tone, where any level or
length error is unmissable: `silenceRange` keeps the clip length, drives the range
to exact zero, leaves everything outside it **bit-identical**, and ramps rather
than cutting at the boundary; `deleteRange` shortens by the range plus one
crossfade, holds its level across the splice, preserves rate/channels, and needs
no crossfade when the cut touches the clip's start or end. Empty ranges and
"delete the whole clip" are rejected. (The splice-level check allows a wide band:
the two sides of that particular test are the same tone a whole number of cycles
apart, so they add coherently and equal-power weights give up to +3 dB. Real
splices join unrelated room tone, which is the case equal-power is chosen for.)

Selection-scoped processing (`dsp::blendProcessedRange`) is covered on the same
synthetic signal: length / channel count preserved, audio outside the range
**bit-identical** to the original, the processed result kept exactly inside the
range past the crossfade edges, a non-voice event inside the range removed
(−60 dB) while one outside is untouched (0.00 dB), empty and mismatched-buffer
inputs rejected, and `isolateVoice`'s statistics following the requested range
(a voice-free 0.6 s window reports 0 segments and 0.6 s removed).

Voice-isolation coverage builds a synthetic 5.2 s signal — harmonic speech-like
stretch (F0 140 Hz + 12 harmonics), a 60 Hz decaying thump, a broadband shuffle
burst, room tone throughout — and asserts: length preserved, speech kept (100%),
bump / shuffle / room tone each attenuated ≥ 20 dB (measured −60 dB, i.e. the
full requested reduction), exactly one detected voice segment, and
reduce + residue == original. It also logs the throughput (≈37× realtime).

## Undo / scopes

Voice cleaning (any algorithm) and voice isolation replace clip buffers and
commit **one snapshot per operation** — a track-wide or project-wide clean /
isolation is a single undo step.

## Unsaved-changes guard

`Document` remembers the undo node that was current at the last save/load/new
(`savedNode_`) *and* the selection at that moment (`savedView_`), both set by
`markSaved()` in `init`/`saveProject`/`loadProject`. `isModified()` is
`undo_.current() != savedNode_` **or** the selection differing from
`savedView_`, so undoing back to the saved state clears the dirty flag and
redoing away re-sets it — no separate dirty bit to keep in sync with the history
(see *Project view state* for why the playhead is excluded). The UI shows a `*`
in the title bar while
modified (refreshed from `onTimer` when the flag flips) and calls
`App::confirmDiscardChanges()` — a Yes/No/Cancel *"Save changes…?"* box — before
any project-discarding action (`WM_CLOSE`/exit, opening another project). Cancel
aborts the action; Yes proceeds only if the save actually succeeds.
