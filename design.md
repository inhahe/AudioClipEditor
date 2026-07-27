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
| `waveform.{h,cpp}` | GDI oscilloscope: min/max envelope zoomed out, per-sample trace zoomed in |
| `dialogs.{h,cpp}` | Manual modal dialogs: text prompt, export options, voice-cleaner options, file/project pickers |
| `layout.h` | Pure drop geometry for the reflowing library grid (`insertIndex`, `caretAnchor`) — split out of `ui.cpp` so it is headlessly testable |
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
- `Document::loadProject` validates on the way in: a selection whose clip is gone,
  or whose bounds fall outside the clip (e.g. the clip was cropped in another
  session), is dropped rather than restored as a bogus highlight.

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
- **`togglePreview(clipId, useSel)`** is the play/pause button for both the
  library card (`togglePlayClip` → `useSel = true`, so the button auditions the
  selection when there is one) and the clip editor (`EB_PLAY` → whole clip,
  `EB_PLAYSEL` → selection). Two rules keep it predictable: **any playing preview
  of this clip pauses**, whichever button was pressed (so the pause button really
  pauses a selection audition instead of restarting it), and **resume happens in
  place only when the armed source still matches** what was asked for
  (same clip, same range) and no seek is pending — otherwise it re-arms at
  `previewCursor`.
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
the next row's head share an index but must draw different carets), the
unsaved-changes flag
(an edit or a selection change dirties the project, saving clears it, moving the
playhead does not dirty it), and the `BufferSource` sub-range behaviour the clip
preview depends on (span, begin-relative seek/position, rendering from the seek
point, stopping and draining at the range end).

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
