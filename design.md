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
| `model.h` | `Clip` (id, name, buffer, peaks, gain, **timestamp**), `Track` (id, name, gain, placed clips, plus the pure lane arithmetic `gapBefore`/`ripple`/`overlaps`), `PlacedClip` (clipId + start frame), `Project` (library order = display order) |
| `decoder.{h,cpp}` | MF Source Reader → stereo float at the project rate |
| `encoder.{h,cpp}` | WAV writer (manual RIFF; 16/24-bit PCM, 32-bit float) + MF Sink Writer (MP3/AAC/WMA); output-rate resampling |
| `engine.{h,cpp}` | WASAPI shared-mode render thread; `BufferSource` (single-clip preview) and `TimelineSource` (all-tracks mix); linear resample project→device rate on the audio thread |
| `undo.h` | Snapshot-based undo **tree**: every edit stores a full `Project` copy; redo with branch picker; `chain()` flattens the walkable line for the History window and `gotoNode()` jumps to any point on it; `allNodes()`/`rebuild()` round-trip the whole tree through the project file, `hasSnapshot` marks steps stored by name only, and `~UndoNode` tears down iteratively so a long history can't overflow the stack |
| `document.{h,cpp}` | Owns `Project` + undo tree + `ViewState`; all mutations go through `commit(desc)`; tracks the last-saved undo node for the unsaved-changes flag (`markSaved`/`isModified`); `history()`/`gotoHistory()` expose the step list |
| `dsp.{h,cpp}` | Radix-2 complex FFT, speech-aware loudness, three noise-reduction algorithms, voice isolation, LTAS timbre matching, manual region edits (see below) |
| `waveform.{h,cpp}` | GDI oscilloscope: per-column min/max envelope (one `PolyPolyline`) zoomed out, per-sample trace zoomed in |
| `dialogs.{h,cpp}` | Manual modal dialogs: text prompt, export options, voice-cleaner options, history list, file/project pickers |
| `layout.h` | Pure geometry split out of `ui.cpp` so it is headlessly testable: the reflowing library grid's drop targets (`insertIndex`, `caretAnchor`), toolbar row wrapping (`flowButtons` for the editor toolbar, `flowTransport` for the main transport bar), scrollbar sizing (`scrollBarsNeeded`, `scrollThumb`, `scrollFromThumb`), and `scrollToReveal` |
| `transport.h` | Pure play/pause decision logic, likewise split out to be testable: `decide(State, Press)` → pause / resume / restart-from-where, for both the single combined control and the editor's labelled button pair |
| `selhistory.h` | Pure undo/redo state machine for the waveform selection, kept out of the document's snapshot tree; a `base` token ties the stack to a point in the document history |
| `snap.h` | Pure directional-snapping state machine for dragging clips along a lane: pulls only when moving *away* from a target, so snapping stays convenient without making near-miss positions unreachable |
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

## DSP: timbre matching (`dsp.{h,cpp}`)

Sentences recorded in separate takes drift in tone even when nothing obvious
changed: a few centimetres of mic distance moves the low end (proximity effect),
a few degrees off-axis rolls off the top, a different position in the room
recolours the mids. All of that is one slowly-varying difference in the clip's
**long-term average spectrum** (LTAS), so one gentle static EQ curve per clip
fixes it.

**The model.** Averaging a clip's power spectrum over all of its speech cancels
the *content* — which words happened to be said — and leaves the recording
chain's colouration. The ratio between two clips' LTASs is therefore exactly the
EQ that turns one clip's tone into the other's, and applying it is the whole
algorithm. This is precisely parallel to `normalizeClips`, which matches one
number (loudness); this matches the whole curve.

**What it can't fix**, because these are not spectral-average differences:
reverb and early reflections (a room tail is a *time* difference — EQ cannot add
or remove one), differing background noise (that's the voice cleaner's job),
clipping, and differences in delivery. The dialog says so.

| Function | Role |
|---|---|
| `computeTimbreProfile(buf)` | measure one clip's LTAS → `TimbreProfile` (per-bin mean `ln(power)`) |
| `averageTimbre(profiles)` | per-bin mean of the log spectra = **geometric mean of power** |
| `matchTimbre(buf, target, opts, curveDbOut)` | filter a clip onto `target`; reports the applied dB curve |
| `timbreDistanceDb(a, b)` | how far apart two tone colours **measure**, in dB; `-1` if not comparable |
| `timbreBinHz(rate)` | Hz per bin, so a caller can say *where* the biggest correction landed |

Framing is 2048/512 — deliberately identical to the voice detector's, so
`detectVoiceFrames`' per-frame mask lines up frame for frame with the analysis.

### Decisions worth keeping

- **Speech frames only.** Room tone differs between takes too, and a clip with
  longer pauses would otherwise be dragged toward its own noise floor — matching
  noise floors is not what "the same timbre" means. Fallback chain when the
  detector finds under 4 voiced frames: the loudest frames (`speechLoudness`'s
  same "within ~20 dB of peak" rule), then all frames. An unusual clip is still
  matched rather than silently skipped: a slightly worse measurement of it beats
  leaving it as the one clip that still sounds different.
- **Mean of powers, then log** — not mean of logs. The log of an average is
  dominated by the loud frames, which is what a tone colour should be; a mean of
  logs weights a near-silent frame's noise floor as heavily as a vowel.
- **The correction is band-limited to roughly 90 Hz – 11 kHz** (`tmBandWeights`),
  raised-cosine ramps in *log* frequency, upper edge following Nyquist so a
  22.05 kHz or 16 kHz source isn't corrected to the edge of its own bandwidth.
  Outside that band a speech recording is mostly its own noise floor — rumble,
  HVAC, handling and desk thump below, mic/preamp hiss above — so the ratio there
  compares one clip's *noise* to another's, which is the one thing this effect
  exists not to do. (Measuring on speech frames only already refuses to match the
  silence *between* words; this refuses to match the parts of a speech frame that
  are not speech.) Unweighted, those bins run away with the whole curve: a trace
  of DC offset, or a fan in one room and not the other, is a 20 dB difference at
  30 Hz — vastly larger than any real difference in tone — and the filter would
  dutifully apply it, boosting one clip's rumble to match another's while
  changing nothing audible. It also poisoned the reported "largest correction",
  which is a maximum over the curve: a real match that moved the audible spectrum
  by 3 dB would report **23 dB**.
- **The band weight is applied to the smoothing average, not just afterwards.**
  Fading the curve out of band after smoothing is not enough, because the
  smoothing window drags the out-of-band garbage inwards first: with the 60 Hz
  floor, the window at 70 Hz spans about 10–130 Hz, so a fan at 30 Hz still sets
  the correction at 70 and 100 Hz — *inside* the band, where the fade can no
  longer remove it. `tmSmoothOctaves` therefore takes the weights and computes a
  weighted mean, so untrusted bins never enter the average and transition bins
  contribute in proportion to how much they're trusted. Measured on the selftest's
  rumble case: after-the-fact fading alone left **6.2 dB** at 70 Hz; weighting the
  average leaves **1.4 dB**.
- **Fractional-octave smoothing with an absolute `minHz` floor (60 Hz).**
  Constant-Q is the right shape (hearing resolves frequency logarithmically), but
  at 100 Hz half an octave is only a couple of FFT bins — narrow enough to
  resolve a voice's individual **F0 harmonics**. Two takes are never at the same
  pitch, so a curve that fine would try to EQ one take's harmonic comb onto
  another's: a violent, warbling correction with nothing to do with timbre. The
  floor averages the low end across several harmonics, leaving only the envelope.
  Smoothing is arithmetic in dB = geometric in power, so a +6 dB bump and a −6 dB
  dip average to 0.
- **The curve is forced to average 0 dB, weighted by the target's power.** A
  broadband offset is *loudness*, not timbre; leaving it in would make this
  quietly double as a normalizer and undo levels the user had already set. The
  weighting matters: an unweighted mean over linearly-spaced bins is dominated by
  the near-empty top half of the spectrum, where the correction is noise, and
  would bias the whole curve to cancel it.
- **De-mean, fade, then clamp — in that order.** Clamping last means the limit
  applies to the curve actually applied: not to a shape plus an offset about to
  be removed, and not to a value at 30 Hz about to be faded to nothing. The band
  weight also goes into the de-meaning weight (alongside the target's power), so
  bins the correction is about to discard get no say in the offset either.
- **Zero-phase application.** The gain is real and mirrored onto the conjugate
  bins (`k` and `kTMWin-k`) of each STFT frame, so phase is untouched and nothing
  is smeared in time. Overlap-add uses the same lead-padded, `sum(w²)`-normalised
  pattern as `denoiseChannelProfile`, which reconstructs exactly — pinned by the
  selftest that matches a clip to its own profile and asserts the samples come
  back unchanged.
- **Measured on the mono mix**, but the correction is applied identically to
  every channel: timbre is a property of the source, so the stereo image survives.
- **Scale, don't clip**, if a boosted band pushes a peak past full scale. Trading
  a subtle tone difference for audible distortion is a bad bargain.
- **The residual is a separate measurement from the correction.** How far a clip
  was *moved* and how close it *ended up* are different questions, and only the
  second one answers "did this work" — a large correction that landed is a
  success, a small one that was clamped is a failure, and the applied curve
  cannot tell them apart. `timbreDistanceDb` is that second number: the RMS of
  the dB difference between two profiles over 100 Hz – 10 kHz, de-meaned (a
  broadband offset is loudness) and weighted **1/f so every octave counts
  equally**. The weighting is the same argument as the de-meaning weight, from
  the other side: linearly-spaced bins put four fifths of their number above
  3 kHz, so an unweighted figure would be almost entirely a statement about the
  top octave. Returns `-1` for profiles that aren't comparable, which lets the
  caller feed it a whole array including invalid entries without filtering first.

### Timbre matching in the app

`App::tmOpts` + `App::tmReference` (session-persisted) → `matchTimbreTrack` /
`matchTimbreAllClips` funnel into `matchTimbreClips(ids, scopeLabel)`. One
options dialog (`dlg::timbreMatch`), then every target clip is filtered and
committed through `Document::replaceClipBuffers` as **one undo step**. Menu ids
`IDM_TIMBRE_ALL` (clip menu, top level) and `IDM_TRK_TIMBRE` (track header menu).

Three ways this differs from the other two effects, all following from it being a
**set** operation rather than a per-clip one:

1. **No "this clip" scope and no selection-only variant.** There is nothing to
   match a lone clip *to*, so both entries are greyed below two clips. A tone
   colour is a property of a whole take, and EQ'ing half a clip differently from
   the other half would create exactly the audible seam this exists to remove —
   which is why `blendProcessedRange` is not used here.
2. **The dialog also picks the reference**: the average of the set (default) or
   one named clip ("Sound like 'take 2'"). The average is the geometric mean, so
   it moves each clip as little as possible and favours none — the same reasoning
   as `normalizeClips`' geometric-mean-of-loudness target.
3. **`tmReference` is stored as a clip *name*, not an index.** The choice
   outlives the dialog that made it and the set it was chosen from changes (a
   different track, a clip renamed or deleted); an index into last time's list
   would silently come to mean a different clip.

All profiles are measured *before* anything is filtered, because the target may
be their average and a clip that can't be measured must be left alone rather than
filtered by a curve derived from nothing. Clips of a sample rate other than the
first one's are skipped — spectra are only comparable bin-for-bin at one rate —
and the summary box names the count, since a clip missing from the match is
exactly the one that will still sound different. The box also reports the largest
correction applied — **at what frequency and to which clip**, since "8 dB at
250 Hz" is a mic distance and "8 dB at 40 Hz" is a fan, and since across two
dozen takes the figure is set by a single outlier that the user can go and look
at — and says so explicitly when the clamp bit, so a partial match doesn't read
as a broken one. When the clamp that bit is the effect's own 24 dB ceiling rather
than a lower value the user chose, the message says *that* instead of sending
them back to a field that cannot go any higher.

**Numeric fields snap to their range on `EN_KILLFOCUS`** (`vcSnapEdit`), and the
range is written into the label — *"Maximum change (0–24 dB)"*. This came from a
real report: a user set the limit to 50, then to 100, and got "24 dB, which is
the limit you set" both times. `vcReadDouble` clamps silently, so the typed value
vanished with nothing on screen ever mentioning a ceiling, and raising the number
could not possibly help. Snapping the box makes the cap discoverable by using the
dialog, and makes every "the limit you set" message true. The ceiling itself
stays at 24 dB on purpose: beyond that a static EQ is no longer matching a tone
colour, it is lifting one clip's noise floor into audibility.

**The summary reports the residual, not just the correction.** After filtering,
every result buffer is profiled a second time and `worstTimbrePair` picks the two
clips furthest apart by `timbreDistanceDb`, before and after — *"‘take3’ and
‘take5’ were 7.2 dB apart; afterwards the furthest apart are ‘take1’ and ‘take4’,
0.9 dB."* The worst pair rather than an average, because an average hides one
stubborn clip among five that agree and that clip is exactly the one the ear
picks out; both ends are **named** so the number can be checked by ear against
the clips it describes.

This exists because "I matched the timbre and they still sound different" has two
completely different causes and the user cannot tell which they have. A residual
still above 2 dB means the EQ was held back (the box says: raise the clamp, or
lower Smoothing to let the curve follow finer detail). A residual near 0 means
the clips now *measure* alike, so whatever is still audible is not a
spectral-average difference at all — reverb, compression on some clips but not
others, or background noise — and the box says that instead, because re-running
the same match with the same settings is otherwise the obvious next thing to try
and it cannot possibly help. The cost is one extra profiling pass over the audio,
which is worth it: without the number, distinguishing the two cases is guesswork.

## Applying an effect to the selection only

Both the voice cleaner and remove-non-voice can be limited to the current
waveform selection instead of the whole clip. Timbre matching cannot — see above.

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

## Transport bar layout (`App::layoutTransport`, `layout::flowTransport`)

The bar is three groups: transport actions from the left (**+ Add Files**,
**+ Add Track**, **▶ Play All**, **■ Stop**), the position/length readout in the
middle, and the history group pinned to the right (**Scale** slider, **Undo**,
**Redo**, **≡ History**). Their positions used to be computed independently from
the two edges, which works only as long as they happen not to meet. Adding the
History button broke that: at the old default 1500 px window on a 150% DPI
display the client is 1478 px while the content wants ≈1653, and the readout —
drawn with `DT_LEFT` and no ellipsis — was silently cut off mid-digit
(`0:00.00 / 0:2`). Nothing else on the bar has that failure mode, because
everything else is a button whose label fits its own box.

`layout::flowTransport` assigns each group a **row** greedily: the left group is
always first on row 0; the middle group stays on the current row if it fits after
it and drops to a new row at the left margin if it doesn't; the right group is
right-aligned on whatever row it lands on, dropping again if that would collide.
`layoutTransport` turns those rows into rects and publishes the total height as
`App::transportH`, which `computeLayout()` reads **before** sizing anything else
— the tracks pane and library start under whatever height the bar came to. The
readout stretches to fill its row (up to the *Scale* caption when it shares one),
so a longer arrangement has somewhere to put its digits.

This is the same principle as the editor toolbar's `flowButtons`: a control that
runs off the edge is unreachable, and a number that runs off the edge is
misinformation, so the bar grows downwards instead. It costs pixels; clipping
costs meaning. Keeping the row assignment in `layout.h` lets the selftest sweep
every plausible client width at 100/125/150/200% DPI and assert that the readout
is never clipped, the history group never starts off the left edge, and three
rows is the worst case.

The **default window is 1700×950** (`WinMain`) rather than 1500×950 so that the
out-of-the-box toolbar is one row at 150% DPI — wrapping is the fallback, not the
normal state. (Window size and position are persisted in
`HKCU\Software\AudioClipEditor\WindowPlacement`, so the default only applies on a
first run.)

## Timeline: scrolling a long arrangement

An arrangement longer than the window used to be reachable only by rolling the
mouse wheel over the lanes. That worked, but nothing on screen said so, there was
no indication of how much arrangement lay off-screen or where in it you were, and
during playback the playhead simply left the window and never came back.

Three things address that, all keyed off `tlScrollX` (which already existed):

- **A horizontal scrollbar** along the bottom of the tracks pane. It spans the
  *lane* area only, starting at `trackHeaderW`, so it sits under exactly the
  content it scrolls rather than under the fixed header column. Clicking the
  gutter beside the thumb jumps there *and* begins a drag, so click and drag are
  one gesture (matching the vertical bar).
- **Keyboard**: `←`/`→` step, `Ctrl`+`←`/`→` and `PgUp`/`PgDn` page,
  `Home`/`End` jump to the start/end. `End` matters most — it is the only quick
  way to find where an arrangement actually stops.
- **Playhead follow** during timeline playback (`followPlayheadIfPlaying`).

**Follow pages rather than recentres.** Scrolling continuously at playback speed
makes the waveforms crawl sideways and is markedly harder to read than an
occasional jump, so the view only moves when the playhead reaches a margin near an
edge, and then jumps by most of a screen.

**Manual scrolling wins.** Any hand scroll — wheel, scrollbar, or key — clears
`followPlayhead`, because scrolling during playback means you have deliberately
gone to look somewhere other than where the audio is; without this the next timer
tick would haul the view straight back and manual scrolling would appear broken
while playing. It is re-armed by starting playback and by clicking to move the
playhead (both are statements about where you want to be watching).

### Two bars, one pane

`layout::scrollBarsNeeded` resolves bar visibility over **two passes**, because
each bar steals space from the other: a horizontal bar shortens the pane and can
be exactly what pushes the tracks over into needing a vertical one, and a vertical
bar narrows the lanes and can force a horizontal one. A single pass silently
clips a strip of content in those cases. Both bars then share
`layout::scrollThumb` / `scrollFromThumb`, which is also what guarantees the thumb
reaches both ends exactly and never shrinks below a grabbable minimum — the case
that matters precisely when the arrangement is longest.

Lane content is clipped to `laneClipBottom()` so it stops above the horizontal
bar instead of running under it, and both bars are painted *after* the lane clip
is released (drawing them inside it would clip the horizontal one away). Hit
testing checks both bars before the lanes, since the lane rects extend underneath
them.

`paintTimeline` uses a **second, nested clip region** for everything that lives in
lane coordinates — placed clips, both kinds of drag ghost, the playhead — set to
start at `trackHeaderW` rather than at the pane's left edge. Scrolling right puts
the start of an already-started clip at a negative lane x, and without the inner
clip that block paints straight across the fixed track-header column, swallowing
the track's name and volume slider. The headers and lane backgrounds are drawn
first, under the outer (whole-pane) clip, and the inner one is released before the
scrollbars.

### Revealing a clip that was added off-screen

`revealPlacement(trackId, startFrame, endFrame)` scrolls the tracks pane in both
axes so a just-placed clip is on screen. The clip context menu's *Add to…*
(below) appends at
the end of the track, which on any arrangement longer than the window is past the
right-hand edge — and on a project with more tracks than fit, the target lane can
be past the bottom edge too. Without this the command looks like it did nothing,
which reads as "adding to the timeline is broken" rather than "the clip is over
there"; a toast naming the track and the time it landed at says the same thing in
words. A clip already fully visible does not move the view at all, since a jump
you did not need is its own kind of confusing, and a clip wider than the lane
lines up its *start* (no scroll position shows all of it). Like any deliberate
view move it clears `followPlayhead`. Dropping a card by hand deliberately does
*not* call it: the drop lands under the cursor, which is on screen by
construction.

### Naming the destination — `IDM_ADDTL_BASE`

The item used to be an unconditional **Add to timeline ▸** submenu, which was
wrong twice over: "timeline" is not a place a clip can go (a *track* is), and
with a single track it made you open a submenu to choose from one entry. The menu
is now built from the track list — one track gives a flat `Add to “Track 1”`,
several give an `Add to track ▸` submenu of names — so the command always says
where the clip is about to land. Command ids stay `IDM_ADDTL_BASE + trackIndex`
either way, so the handler is unchanged.

That handler is also where the dangling-reference rule bites: `tracks` refers
into `doc.project().tracks` and the `Clip` lives in the library vector, both of
which `placeClip` can reallocate, so the track id, track name, clip name and clip
length are copied out *before* the edit and only the copies are used for
`revealPlacement` and the toast.

There is one more coupling, in `computeLayout()`: the tracks pane is sized *to
just fit* its tracks, so introducing a horizontal bar would push the bottom lane
under it and — correctly, per the two-pass rule — summon a vertical bar as well.
Two bars where one would do reads as a bug. So `computeLayout()` adds `sbT()` to
the pane's needed height when the arrangement is wider than the lane area, and
the pane then fits its tracks *plus* its bar. It tests against the full lane
width (i.e. assuming no vertical bar), which is the right assumption for a pane
that fits its tracks; when the pane is instead clamped short to keep the library
visible, `tlBars()` resolves the pair as usual.

## Timeline: moving a placed clip (drag feedback)

`Mode::ClipMove` (started in `onLDown` when `placedAt(p)` hits) drags a placed
clip. During the drag `paintTimeline` draws a **live ghost** of the clip (name +
waveform) at the snapped target position under the cursor — coloured
`clipBlkSel` when the drop is legal, `stop` (red) when it would overlap — while
the clip's home slot is left as a faint outline. The actual `Document::moveClip`
happens on `onLUp`. The ghost mirrors the existing `Mode::CardDrag`
(library-card → track) drop preview and shares the snap result and
`Track::overlaps(start, len, ignore)` check with the commit, so what you see is
where it lands.

### Directional ("sticky") snapping — `snapping::Sticky` (`snap.h`)

Snapping used to be symmetric and stateless: `snapFrame` took the raw position
and returned the nearest interesting frame within ~10 px. Convenient, but it made
a band around every snap point **unreachable** — you could not leave a 3-frame gap
after a neighbour, because getting that close was exactly what triggered the jump
flush against it. The gap you wanted and the snap you wanted were indistinguishable
to the drag, and the snap always won.

So the pull is now applied in one direction only:

- Moving **toward** a target does nothing at all — the clip tracks the mouse, so
  any position, however close to a neighbour, is reachable.
- **Reaching or crossing** a target engages it: the clip parks exactly on it. This
  is how you land flush, and it needs no aim — anywhere past the point will do.
- Moving **away** from an engaged target holds the clip there, resisting, until
  the mouse is more than `tol` away; then it lets go and jumps to the mouse.

Snapping therefore still costs nothing to use (overshoot slightly and you are
flush — the common case), while every position stays reachable by approaching from
the far side. The deliberate cost is that pulling free jumps by `tol`; that pop is
the feedback that you have left the snap, and it introduces no drift because the
clip is back under the mouse afterwards.

**This makes snapping path-dependent, which has one structural consequence:** the
result can no longer be re-derived from the cursor position, because it depends on
how the cursor got there. `Sticky::update` must be called *exactly once per pointer
update*, so it lives in `onMouseMove` alone (`updateDragSnap`) and stores its
answer in `dragSnapStart`; `paintTimeline` and the `onLUp` drop both *read* that
rather than recomputing. Painting used to call `snapFrame` itself — doing that now
would step the state machine on every `WM_PAINT`, including repaints no user action
caused. `update` is idempotent for a repeated position so the final `updateDragSnap`
in `onLUp` (which guarantees the drop matches the ghost) is inert in the normal case.

`snapTargets` supplies the candidates — flush after each neighbour, flush before
each neighbour, and frame 0 — excluding the dragged clip itself via `ignoreIndex`,
and dropping "flush before" candidates that land below 0 since those are not
reachable positions. `dragSnap.begin()` seeds the state at the drag's start with
the clip's **current** position, which is what makes an already-flush clip resist
the first nudge.

`updateDragSnap` takes the drag's `Mode` as an argument instead of reading the
member. `onLUp` clears `mode` *before* landing the drop (a message box during the
drop pumps messages, and a repaint then would draw a ghost for a drag that is
over), so reading the member made the final, drop-deciding update behave as if it
were not a clip move — the clip could snap to the edges of its own old position,
and a ripple lost its lane.

## Timeline: changing the space between clips (ripple)

Moving a clip normally changes **two** gaps: the one in front of it opens or
closes, and the one behind it does the opposite. That is wrong whenever the
arrangement downstream has timing worth keeping — a row of takes, say, where you
only want to widen the pause before take 3 and leave the rest as recorded.

A **ripple** is the fix, and it is deliberately a tiny idea: *rigidly translate
the suffix of a track's clip list starting at index `i`*. Distances inside the
suffix are preserved by construction, so exactly one gap changes — the one in
front of clip `i`. The only free parameter is the delta.

`Track::ripple(index, delta)` (`model.h`) is the whole operation, and
`Track::gapBefore(index)` is how a gap is named (distance from the previous
clip's end, or from frame 0 for the first clip). The delta is clamped to
`-gapBefore(index)`: sliding left stops flush against the clip in front, since
there is nowhere further to go without overlapping. Sliding right is unbounded —
the timeline has no end. Because this translates a whole suffix and the clamp
keeps it behind the prefix, **it cannot reorder or overlap anything**, so unlike
`moveClip` it needs no collision test and no `sortClips()`. `ripple` returns the
delta actually applied; `Document::rippleClips` commits an undo step only when
that is non-zero, so a drag that lands back where it started leaves no history.

Four ways in, all funnelling into `rippleClips`:

| Gesture | Delta |
|---|---|
| **Shift+drag** a placed clip | wherever the drag ends up, minus where the clip started |
| Drag a placed clip with **Timeline ▸ Ripple drag** ticked | same, no modifier needed |
| Right-click → **Space before this clip…** | requested gap (typed in seconds) minus the current one |
| Right-click → **Close the space before this clip** | `-gapBefore(index)` (the clamp does the work) |

A ripple drag reuses the whole `Mode::ClipMove` machinery, with three differences,
all following from what a ripple *is*:

- **It stays on its lane.** A ripple is defined by one track's ordering ("this
  clip and the ones behind it"), so `updateDragSnap` skips `trackAtPoint` and the
  drop never calls `moveClip`. Dragging to another lane while rippling would mean
  "move this clip elsewhere *and* close the gap here" — a different, compound
  operation.
- **The clips it carries are not snap targets.** `snapTargets` takes an
  `ignoreAfter` flag that drops every index past the grabbed one; a clip that is
  moving with you is not a fixed point to land on.
- **It cannot be refused.** An ordinary move that collides is rejected with a red
  ghost; a ripple is a continuous adjustment of one gap that bottoms out at zero,
  so `dragMinStart()` clamps the ghost flush against the clip in front instead,
  matching `Track::ripple`'s own clamp.

**Where the Shift comes from, and when it is read.** `rippleDrag` is set from the
mouse message's own `MK_SHIFT` bit (`GET_KEYSTATE_WPARAM(wp)`), never from
`GetKeyState`. `GetKeyState` reports only the key state *this thread has already
dequeued*, so a Shift that went down while the window did not have focus — the
common case, since you reach for the modifier on the way to clicking — reads as
up, and the gesture silently degrades into an ordinary move that the next clip
blocks with a red ghost. The message's flag is captured by the OS when the click
happened and is always right.

`shiftHeld(msgFlag)` ORs that flag with `GetAsyncKeyState(VK_SHIFT)`, which reads
the physical key *now* irrespective of focus or queue state. The two sources fail
in opposite directions — the message flag is exact for the instant of that click
or move but blind to a Shift pressed between messages; the async state is blind to
a Shift that was down at click time but released before the read — so either one
alone can miss the gesture and the pair does not.

**Shift latches.** Shift at any moment of the drag — the click, any pointer move,
or Shift's own `WM_KEYDOWN` (which matters when the modifier arrives after the
drag started and the pointer then stops moving) — calls `latchRippleShift()`, and
nothing turns it off again until the drag ends. Releasing Shift does *not* revert
the drag. That asymmetry is deliberate: a ripple is chosen on purpose and always
succeeds, while the plain move it would fall back to can be refused by the next
clip on the lane, so a momentary "Shift looks up" — a screenshot hotkey, a focus
change, a swallowed key event, or simply letting go early — would silently turn a
working gesture into a blocked one, which is the one failure the user cannot see
coming. Esc still cancels the whole drag. Latching mid-drag pulls the ghost back
to the home lane, the visible consequence of a ripple being lane-locked.

**Discoverability is the hard part of this feature, not the arithmetic.** The
ripple was reported as broken twice, and both reports turned out to be the same
thing: a modifier is invisible. Nothing on screen said the gesture existed, so
the drag did what a drag does, hit the next clip, and went red — behaviour that
looks exactly like a bug when you don't know the other mode is there. The
arithmetic had been right the whole time. Four answers, all of which put the
gesture somewhere it can be *seen*, and the last two of which put it at the
moment of failure:

- **`Timeline ▸ Ripple drag`** — `rippleMode`, a standing, session-scoped
  preference, with `rippleDrag == rippleMode != rippleShift`, so Shift *inverts*
  the mode rather than forcing ripple on and stays an escape hatch in both
  directions. A menu item is a feature you can find by looking; it is also what
  you want when a whole session is spacing work, and it gives the gesture a route
  that does not depend on the keyboard at all.
- **The same toggle in a placed clip's right-click menu**, next to *Space before
  this clip…* and *Close the space before this clip* — because that is the menu
  someone spacing out an arrangement actually opens, and those two neighbours are
  what brought them there. The menu-bar **Arrange** popup has nothing else in it,
  so it is a menu with no reason to be opened; a feature reachable only from there
  is a feature nobody finds. (It was called *Timeline* until a user opened it
  looking for the edit history — "the timeline of what I've done to this
  project" — which is the History window; see below.) Both routes go through
  `App::toggleRippleMode`, which
  owns the menu-bar tick and a toast naming the direction the toggle just went (a
  menu you dismissed to read its own tick is a poor way to learn what you did).
- **The plain-move badge names the other gesture**: `move • +0.42 s • Shift:
  carry the later clips too`, dropped once Shift has been used in that drag,
  since by then it is known. A hint costs nothing at the one instant the reader
  is certainly looking at the clip and thinking about where it goes — which is
  worth more than any amount of it being written in the Controls dialog.
- **The blocked drag says why, and says the way out.** When the grabbed clip's
  target overlaps, the ghost goes red and the badge stops reporting a distance
  nothing will move by: `won't fit — the next clip is in the way • Shift: carry it
  and the rest along`. And the **drop itself is no longer silent** —
  `Document::moveClip` returns false when it refuses, a return `onLUp` used to
  discard, leaving the clip springing back with no explanation, which is precisely
  what a broken drag looks like. It now raises the undo/redo toast instead. Both
  messages are conditioned on the obstruction being on the clip's **own** lane:
  a ripple is lane-locked, so it is no answer to "that other track is already
  full", and offering it there would be a false lead.

**The ghost says which gesture it is.** A ripple and a move look alike until they
land, so the grabbed clip's ghost carries a one-line badge: `move • +0.42 s`, or
`ripple • gap 1.35 s • carrying 4 clips`. Besides being the readout you want while
setting a gap by eye, it is the only direct evidence that the modifier was
received — the previous bug was invisible except as a colour nobody could explain.

The ghost shows **every** carried clip, not just the grabbed one, since the point
of the gesture is that the tail moves too — `dragCarries(trackId, index)` decides
membership for both the ghosts and the faint home-slot outlines, so the two can't
disagree. `drawClipBlock` draws a clip block for both the real lane and the
ghosts, so a ghost cannot come to look like something the drop won't produce.

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

### Split selection out to a file (the one-click variant)

`App::splitSelectionOutToFile(clipId)` (`IDM_SPLITOUTSEL`) is *new clip from
selection* + *export* + *delete clip* run back to back. It exists because that
sequence is a single intention — **keep this part of the take, throw the take
away** — that otherwise costs three menu trips and four dialogs (name prompt,
export options, Save As, delete confirmation) whose answers are the same every
time. So the command asks nothing and decides them all:

- **Name:** `<clip> (selection)`, the same convention *Export selection…* already
  suggests.
- **Format:** WAV 24-bit at the clip's own rate and channel count — the same
  "don't resample, don't upmix" defaults `exportClipAudio` uses.
- **Directory** (`App::defaultExportDir`): the folder the clip's `sourcePath` is
  in, else the project's folder, else `CSIDL_MYMUSIC`. Each is a place this
  material already lives; the process's working directory is not (it's wherever
  the `.exe` was launched from).
- **Collisions** (`mfio::uniqueFilePath`): steps to `name (2)`, `name (3)`… With
  no Save As dialog there is no overwrite prompt, so nothing already on disk may
  be written over — the new name gets out of the way instead. After 999 tries it
  falls back to a `GetTickCount` suffix.

Ordering is load-bearing: **the file is written before the project changes**, so a
failed encode leaves the clip exactly where it was and there is nothing half-done
to undo. The project half is `Document::replaceClipWithNew`, which is one undo
step rather than an add followed by a remove — the intermediate state (new clip
present, old clip still there) is one the user never asked for, and stopping there
on undo would be wrong. The new clip takes the old one's **library slot** (so it
appears where the eye already is) and its **gain**, and carries the path just
written as its `sourcePath`, which makes it a normal file-backed clip: a later
*Export clip* defaults to the same folder, and sort-by-time uses the file's own
mtime.

Placements of the replaced clip are removed with it — their audio no longer
exists in the project. That is the one consequence the user might not have
intended, so it is the one thing the command asks about (a Yes/No naming the
count), and only when there are any. Afterwards `afterHistory()` runs, which stops
playback (buffers changed), re-clamps the selection and drops the selection-undo
stack, and a toast reports the written path through `PathCompactPathExW` so the
file name — the part that says whether the collision counter kicked in — survives
the ellipsis.

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
  v1/v2 files simply load with a default (empty) view state. **v4** appends the
  edit history after *that*, on the same principle — see "Persisting the history"
  below. Each version has only ever added a trailer, so every reader understands
  every older file and older readers understand as much of a newer file as they
  have sections for.

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

## Timeline playback — the arrangement snapshot and its fingerprint

The engine is never handed the live project: the WASAPI render thread would then
be reading model data the UI thread is editing. `playAll` instead builds a
**snapshot** — a `std::vector<TimelineSegment>` of `{buffer, timelineStart,
length, gain}` plus the total length — and `TimelineSource` mixes from that.
Gaps need no representation at all: `render` zero-fills the output block before
mixing the segments that overlap it, so silence between clips is what you get by
construction.

The cost of a snapshot is that it goes stale the instant the arrangement changes,
and a stale snapshot is *inaudibly* wrong — it plays a perfectly plausible
arrangement that simply isn't the one on screen. `App` therefore records
**`armedFingerprint`**: an FNV-1a hash of exactly the inputs the snapshot is
built from (per track: id, mute, gain; per placed clip: clip id, start, length,
the clip's gain and buffer pointer and frame count; then the total length). It is
derived rather than maintained, so no mutation site has to remember to invalidate
anything — which matters, since the arrangement can change from clip drags,
ripples, mutes, gain changes, deletions, destructive edits, undo, redo and
history jumps.

Two places consult it:

- **`playAll`** pauses a running arrangement unconditionally (Space must always
  mean pause), but only *resumes* a paused one while the fingerprint still
  matches. Otherwise it falls through and re-arms from the playhead. Without
  this, the sequence "play, Space to stop, drag a clip, click a new play
  position, Space" resumed the pre-drag snapshot, so the drag sounded as though
  it had never happened — the clip's move and the silence it opened were both
  missing. (`engine.resume()` reuses the stored source verbatim, and
  `engine.seek()` merely repositions that same stale source, so the playhead
  landed in the right place inside the wrong arrangement.)
- **`syncTimelineSource`**, called from `onTimer` while playing, re-arms at
  `engine.position()` when the fingerprint moves, so an edit made *during*
  playback is heard immediately rather than at the next Space. Re-arming at the
  heard (not rendered) position keeps the discontinuity to the device buffer.

`armTimeline(atFrame)` is the one place that builds and hands over a snapshot and
is the only writer of `armedFingerprint`; `stopAll()` clears it back to 0. The
selftest builds snapshots the same way `playAll` does and pins both directions:
after a ripple the gap renders as exact silence with the clips either side still
sounding, and a snapshot captured *before* the ripple renders audio right through
where the gap should be — the failure mode itself, held in place so the reason
for the fingerprint can't be optimised away.

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

## Where the selection is drawn — and where it deliberately isn't

**A selection is an editing cursor, not a trim.** There is one selection in the
whole app (`selClipId` + `selStart`/`selEnd`) and it means "what the next
operation applies to". It is scoped to the *library*: the clip card, the
full-window editor's main view, and the editor's fine-tune strips.

It is **not** drawn on a clip's block in a track lane, and that omission is
load-bearing rather than an oversight. It *was* drawn there for one version, on
the reasoning that a selection belongs to the clip and so belongs on every surface
showing that clip. That reasoning is fine in the abstract and wrong in this UI: on
a track lane a highlighted band inevitably reads as *"this is the part that
plays"* — and it isn't. `TimelineSource::render` maps `src = p - seg.timelineStart`
across the whole placement, and `TimelineSegment` is `{buf, timelineStart, length,
gain}`; there is no trim field anywhere in the playback path. The overlay was
advertising a feature that does not exist, and it did in fact mislead — it
prompted "won't the timing be off, and won't multiple tracks then be visually
misaligned?", which is a real problem, but only for the trim semantics the overlay
implied.

The supported way to put *part* of a clip into an arrangement is **New clip from
selection** → drag the new clip onto the track. That preserves the invariant that a
block's width is its audio, so lanes stay aligned, the playhead stays linear, and
there is no per-placement in/out state to serialise.

If timeline trimming is ever actually wanted, the shape is a per-placement
`srcOffset`/`srcLength` on `PlacedClip` (in/out points — no audio duplication,
re-trimmable later), an `.acep` version bump to 4, and a revisit of the placement
clamp in `document.cpp` (`len = min(pc.lengthFrames, c->frames())`). It is a
separate feature from the selection and should not be spelled by overloading it.

`drawSelOverlay(hdc, wv, clip)` remains the single implementation, now used only
by the library card. It takes a rect across which the clip's **whole** buffer is
spread and paints the tinted band, the waveform redrawn in `col::waveSel` inside
it, and an edge line each side. The frame→x mapping uses the full untrimmed `wv`
so it agrees with the `wf::draw` beneath, and a sub-pixel selection is widened to
one pixel so a very short selection still shows. Callers guard with
`selectionCovers(clipId)` rather than re-deriving "does this clip own the
selection".

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

## Selection undo — `selhist::History` (`selhistory.h`)

**Ctrl+Z steps back through selections as well as edits.** A selection is
positioned by hand and one stray click destroys it, so it needs to be undoable —
but it is *not* project state, and putting it in the document's snapshot tree
would copy the whole `Project` per drag and fill the redo-branch picker with
entries that change no audio. So it gets its own stack.

The stack is valid only while the document stays put. Every call carries a
`base` token — the UI passes `doc.historyNode()`, the current undo-tree node,
the same pointer-as-a-bookmark trick `savedNode_` already uses — and when that
changes the entries describe states that are no longer reachable and are
dropped. No hooks into the many `commitEdit` call sites are needed; the check is
self-synchronising. `afterHistory()` additionally calls `resetSelHistory()` so an
undo-then-redo cannot revive a stale stack by landing back on the original node.

That single rule is also what makes the precedence easy to state, and it is the
behaviour to preserve if this is ever revisited: **Ctrl+Z walks back the
selections made since the last edit; once they run out, it undoes the edit
itself.** Undo is never ambiguous about which of the two it is about to do.

Changes are recorded at gesture *end*, reusing the same mouse-down snapshot that
Esc uses (`recordSelChangeFromDrag`), which covers the sweep, the single-edge
nudge, and the plain click that clears — and, because `record` ignores a no-op,
a click that re-picks the same range leaves no dead entry to step through. The
two one-click destroyers, the editor's **Clear selection** button and the card
context menu's *Clear selection*, record explicitly. A drag cancelled with Esc
records nothing: it already put the selection back itself (`edUp` checks
`d != 0`, i.e. that the drag was not already cancelled).

`doUndo`/`doRedo` are the single choke point, so the toolbar buttons and the menu
items behave identically to the keys; the buttons' enabled colour is
`doc.canUndo() || canSelUndo()`, since greying them while Ctrl+Z would still do
something would be a lie. The state machine is Win32- and document-free, so all
of it — including redo-branch invalidation and the base-token rule — is exercised
headlessly in the "selection history" block of `selftest.cpp`.

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

`dlg::timbreMatch(parent, TimbreMatchContext&)` adds one thing the other two
don't need: a **reference combo** listing "The average of them all" followed by
"Sound like '<clip>'" for each clip in the set. The context carries the names in
and the chosen index (`-1` = average) back out. Clip names are clamped to
`S(320)` when sizing the combo so a long name can't stretch the dialog off the
screen. Otherwise it is the same shape as `voiceIsolate`: numeric Maximum-change
and Smoothing edits through `vcReadDouble`, a Keep-loudness checkbox, and the
scope line.

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
placed at an absurd 300 px width), **`layout::scrollToReveal`** (a span already
in view doesn't move the position, one past either edge comes in with its margin,
one too wide to fit lines up its start, both ends clamp to the scroll range, and
a viewport of zero or one smaller than two margins degrades sanely), the
**transport bar wrapping**
(`layout::flowTransport`: one row at the default window at both 100% and 150%
DPI, the history group wrapping first, the readout wrapping rather than being
cut off and restarting at the left margin — plus a sweep of every client width
from 420 to 2600 px at 100/125/150/200% DPI asserting the readout is never
clipped, the history group never starts off the left edge, and three rows is the
worst case — and a text-measurement check that every transport button label fits
its hand-sized box, including *≡ History* and both *Play All* / *Pause*
alternates),
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
rather than `ui.cpp` precisely so it can be. `mfio::uniqueFilePath` sits there for
the same reason and is tested against the real filesystem (the test touches the
files it expects to collide with): plain name when the slot is free, `(2)` then
`(3)` as it fills up, a trailing separator on the directory not doubling up, and
the clip name sanitised on the way in. Its half of *split selection out to a file*
is what stops that command overwriting an earlier export, and it has no dialog to
fall back on. The project half, `Document::replaceClipWithNew`, is checked on a
three-clip library with two placements: the replacement lands in the old clip's
slot, the neighbours don't move, the gain and the written path come across, the
placements of the replaced clip go with it, a nonexistent clip id is not an edit
(it must commit nothing, or the undo check below would land on the wrong state),
and **one** undo restores both the clip and its placement.

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

**Timbre matching** is tested end-to-end on two clips built from the *same*
deterministic noise source, one passed through a known first-order tilt — noise
rather than tones so the long-term average is smooth and the test measures the
matching rather than where harmonics happened to land between bins. The two
profiles start 2.62 dB rms apart (de-meaned, 100 Hz–10 kHz) and matching collapses
that to **0.02 dB**; matching both clips to their *average* converges them on each
other equally well, which is the actual workflow. Matching a clip to **its own**
profile is asserted to be a sample-accurate no-op (worst error 1e-6), which pins
the overlap-add reconstruction as well as the zero-mean logic. Also covered:
loudness preserved to within 1.5 dB (it must not double as a normalizer),
`averageTimbre` equalling the per-bin mean and ignoring a profile of a different
rate, the `maxCorrectionDb` clamp holding exactly, and refusals for an unmeasured
profile / a rate mismatch / a clip shorter than one analysis window.

`timbreDistanceDb` — the residual the summary box reports — is checked to agree
with that convergence (2.19 dB → 0.03 dB on the same pair) and then for the
properties that make the number mean anything: zero distance from a profile to
itself, symmetry in its arguments, `-1` for an unmeasured profile or a rate
mismatch, and — the one that matters — **immunity to a broadband level change**.
Scaling a clip by 0.25 must leave the distance at 0, or the report would call two
identically-coloured takes different merely because one is quieter, which is
exactly the confusion the number exists to remove.

The **band limit** is pinned by a case built to break the old behaviour: `clipA`
against a copy of itself with a loud 30 Hz tone added, i.e. two takes identical
where speech lives and wildly different below it. That must produce essentially
no correction — the assertion is under 3 dB anywhere, and that the largest
correction lands between 60 Hz and 14 kHz. It measured **1.4 dB at 70 Hz**;
before the weights, over 20 dB at 30 Hz. DC and Nyquist are separately asserted
to come out at exactly 0 dB.

**History** (14 checks) is tested for the flags rather than the plumbing, because
a step listed as applied when it has actually been undone is worse than no list
at all: a new project is one current, saved step; three edits list four steps
newest-last with their commit descriptions; undoing twice *keeps the undone steps
listed* (they're still redoable — that is the question being asked) but marks
them not applied; the saved marker sits on the step the file holds and moves with
`markSaved`. Jumping is checked to restore the same state undo/redo would
(including clip gain, which nothing on screen would show), to refuse a jump to
where it already is or off either end, and — the subtle one — to leave a redo
trail that walks *forward the way it came* rather than down an older branch. A
new edit made after undoing is checked to replace the listed tail and to leave
the fork point reporting `branches == 2`.

## Undo / scopes

Voice cleaning (any algorithm), voice isolation and timbre matching replace clip
buffers and commit **one snapshot per operation** — a track-wide or project-wide
clean / isolation / match is a single undo step.

## History window and undo feedback

The problem this solves: an effect changes only how a clip *sounds*. After
matching the timbre of 23 clips, undoing twice and redoing once, nothing on
screen distinguishes "the match is applied" from "the match is undone" — and the
one command whose whole purpose is to change state, `Ctrl+Z`, looks like a key
that did nothing. Two pieces, both built on descriptions the undo tree was
already storing.

**`UndoTree::chain(int* currentIndex)`** returns the single line the user can
walk with the keyboard: root → … → current (everything applied), then on past
current down the remembered `lastChild` trail to the tip (everything undone but
still redoable). Side branches are *not* listed — they're reachable only through
the redo picker — but a listed node with `children.size() > 1` lets the UI say
how many alternatives it isn't showing, so the old ones don't look lost.

**`UndoTree::gotoNode`** is click-to-jump. It re-points `lastChild` all the way
up the path to the target, because jumping has to be indistinguishable from
pressing undo/redo the right number of times: after jumping back, undoing
further and then redoing must return *the way it came*, not down whichever
branch happened to be last followed.

**`Document::history()`** turns the chain into `HistoryEntry` rows (`desc`,
`applied`, `current`, `saved`, `branches`, `restorable`); `historyIndex()` says where we are;
`gotoHistory(index)` moves there. `gotoHistory` takes an **index**, not a node
pointer, and re-derives the chain: the History window hands back a row number it
was given earlier, and a node pointer held across that boundary would dangle the
moment anything committed in between.

**`dlg::history` (`dialogs.cpp`)** is an owner-drawn listbox (`LBS_OWNERDRAWFIXED`)
because the applied/undone distinction has to be visible at a glance and a plain
listbox can only vary the text: applied rows draw in `COLOR_WINDOWTEXT`, undone
rows in `COLOR_GRAYTEXT`, and a marker column (`▶` current, `✓` applied, `↺`
undone) carries the same information without relying on colour. Rows also call
out the saved state ("the saved file holds this") and fork points. The window
**stays open** while the project moves behind it — jumping goes through a
`std::function<int(int)> jump` callback that returns the new current index rather
than the dialog closing with a choice — because answering "was the timbre match
undone?" usually means trying two positions and *listening*, not making one
decision. Reached from the **≡ History** button in the transport bar
(`TB_HISTORY`), from `Edit ▸ History…`, or with `Ctrl+H`.

**Why there is a toolbar button.** The window is where you go to answer "what
have I done to this project", and the two controls that step through that same
list — Undo and Redo — are on the toolbar, so the list itself belongs beside
them. Before, it lived only under `Edit` and `Ctrl+H`, and a user hunting for it
opened the menu-bar **Timeline** entry instead (that menu is about the track
lanes, but its name reads as "the timeline of what I've done"). That menu is now
called **Arrange** for the same reason. The button is dimmed while
`doc.history()` is empty, and the history *description* for placing a clip names
its destination track (`App::placeDesc`) so the list answers the question the
same way the menu asks it: *Add 'ref_10s' to Track 2*, not "to timeline".

### Persisting the history — `.acep` v4 and the buffer pool

The history used to die with the session: `loadProject` called `undo_.init()` and
you got one step. That is backwards, because a session boundary is exactly when
"did I apply the timbre match?" stops being answerable from memory — the window
was empty precisely when it was most needed.

Writing it is not a plain serialisation chore. Every `UndoNode` holds a whole
`Project`, and Projects share their audio through `shared_ptr`, so a twenty-step
history costs almost nothing in memory and would cost twenty copies of the audio
on disk if written the obvious way. So the format gets a **content-addressed
buffer pool** and snapshots reference it by index:

- **Slots `0 … library-1` are the current library's buffers**, which the v3 body
  has already written inline. Slot numbering is the library *position*, not a
  dedup counter, because that is the only numbering a reader can reproduce
  without being told it. A snapshot referring to audio the project still uses is
  therefore free — which is the common case, since most steps (placements,
  gains, renames, moves) change no audio at all.
- **Only superseded buffers are appended**: earlier versions of clips that a
  destructive edit replaced, and clips that were deleted. Identity is the
  pointer, not the contents — that is exactly the sharing the undo tree creates,
  and hashing every sample on every save to find more would cost more than it saves.
- **Loading rebuilds the sharing**, handing one `shared_ptr` (and one `PeakCache`,
  built once per slot) to every snapshot that wants it. So the round trip is
  lossless in the thing that matters: memory usage after a load is what it was
  before the save, not one copy per step.

**The tree itself** is written flat — `allNodes()` walks it breadth-first, which
guarantees parents come before children, so `rebuild()` can reconstruct it from
one parent index per node. Each node carries `lastChild` (the remembered redo
branch) so a fork comes back pointing the way it was pointing. `rebuild()`
rejects anything malformed outright rather than building half a tree: a corrupt
history must cost the history, never the project, which by then is already parsed
and intact. The current node's snapshot is overwritten with the project as read
from the file's own body, so the two can't disagree.

**The budget** (`g_historyAudioBudget`, 512 MB) caps the *appended* audio only.
Destructive effects rewrite every clip they touch, so a handful of them genuinely
does multiply a project's size, and a save that silently turned 200 MB into 3 GB
would be a worse surprise than a shortened history. Nodes are admitted
**nearest-first** — breadth-first from the current node over parent *and* child
links — which is both the right priority and the cheapest one, since recent steps
are the most likely to be wanted back *and* the most likely to share their audio
with the current project. A node that doesn't fit is still written, by **name
only** (`hasSnapshot = false`): the History window can still say the timbre match
happened, it just can't take you back to before it. Everything that can reach a
node checks the flag — `canUndo` stops at the gap, `defaultRedoBranch` skips over
it, `gotoHistory` refuses, and the row draws with a `·` marker instead of a tick
or a rewind arrow, because either of those would promise a state that isn't there.
`Document::setHistoryBudgetBytes` exists so the trimming path can be tested at a
budget of one byte instead of with a half-gigabyte fixture.

**Compatibility runs both ways.** The trailer is appended after everything a v3
reader knows about, so an older build opens a v4 project and simply doesn't see
the history; and `File ▸ Store the edit history in project files` off writes a
v3 file byte-for-byte, for when the superseded audio isn't worth the disk.

**`~UndoNode` tears the tree down iteratively.** Letting `unique_ptr` do it
recurses once per node, and a history is one long thin chain — one stack frame
per edit the project has ever recorded. That was survivable when a history lasted
only as long as a session; now that it accumulates across reopens it grows
without bound, and the crash would land on whoever had used the app longest. The
selftest builds a 20 000-step history and destroys it; without the iterative
destructor that test kills the process (verified).

**Undo/redo toast** (`App::toast`, `paintToast`, expiry in `onTimer`): a
transient badge, centred under the *tracks pane*, naming what just moved — *"Undone:
Match timbre of 23 clips to their average"*. It fires on **every** undo and redo
rather than trying to classify which edits are visible: a heuristic for "was that
change on screen?" would be wrong sometimes, and always-accurate feedback beats
a badge that sometimes stays silent when it mattered. "Nothing left to undo" is
reported too — that's the case most easily mistaken for a broken key. The
description comes from `undoDesc()` before undoing and `redoChildDesc(branch)`
before redoing; selection-history undo (which short-circuits ahead of document
undo) says *"Undone: selection change"*. The badge is measured with
`DrawText DT_CALCRECT` using the same flags it is drawn with, not
`GetTextExtentPoint32` — the two disagree by a pixel or two on overhang, and a
box one pixel short makes `DT_END_ELLIPSIS` eat the last word, which turns
"Nothing left to undo" into a different message.

**Where it sits.** Below the toolbar, so it never covers the Undo button being
clicked repeatedly — and below the *tracks pane* as well (`rcTimeline.bottom`),
because that pane is only as tall as its tracks: with one track it is a thin strip
directly under the toolbar, and a badge placed at `rcTransport.bottom` covered the
ruler and the whole lane. Most toasts are *about* the timeline ("won't fit — hold
Shift…", "Added 'x' to Track 1 at 0:10.00"), so landing on top of the clip they
are explaining is the one place they must not be. Floating over the top of the
library instead costs nothing: it is a scrolling grid and the badge is gone in a
couple of seconds. In the full-window editor it stays at `edToolbarH`, which is
that view's toolbar.

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
any project-discarding action (`WM_CLOSE`/exit, `newProject`, opening another
project). Cancel aborts the action; Yes proceeds only if the save actually
succeeds.

### New Project (`App::newProject`, `IDC_NEWPROJ`, Ctrl+N)

`Document::init` does the document half — fresh `Project` with one empty track,
`undo_.init` so the old history is unreachable (undoing into the discarded project
would resurrect clips the user just threw away), and `markSaved` so a new project
doesn't start dirty. The UI half is everything in `App` that *pointed into* the
old project and would otherwise name something that no longer exists: preview and
playback state, both selections and the selection-save, the hover ids, scroll
offsets, the clip-editor overlay, any drag in progress, `tmReference`, and the
noise captures. Session **preferences** deliberately survive — effect options,
ripple mode — because they describe how the user works rather than this project.

`rate` is restored from `App::nativeRate` (the device rate, floored at 48 kHz,
captured at startup). `rate` follows whatever project is open, so without that a
new project would silently inherit the sample rate of a 44.1 kHz project loaded
earlier and decode every subsequent import at it.

**There is deliberately no separate "Close Project".** The app has no
documentless state — the library and timeline *are* the window — so closing a
project could only ever mean being left in front of an empty one, which is what
New Project already does. A second menu item doing the same thing would suggest a
difference that doesn't exist.
