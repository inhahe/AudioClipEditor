# Known issues / technical debt

Open items only. Fixed things move out of this file; the design rationale for a
fix belongs in `design.md`.

## Bugs

### Broadband non-voice next to speech is still shielded by the gate hold

- **Where:** `dsp.cpp`, `isolateVoice` — the bump-override path that lets an
  event inside the gate's hold window be removed anyway.
- **Symptom:** a thump close to speech is removed (that regression is covered by
  the selftest, measured −21.0 dB), but *broadband* non-voice at the same
  distance — chair scrapes, paper rustle, clothing shuffle — is not. The hold
  keeps the gate open across it.
- **Cause:** the override only recognises **low-frequency** thumps. Its energy
  test looks at the bottom of the spectrum, so an event whose energy is spread
  across the band never trips it and the hold wins.
- **Proper fix:** widen the override into a general "this frame is loud but not
  voiced" test rather than a low-band one — e.g. gate on harmonicity /
  spectral-flatness against the neighbouring voiced frames instead of on band
  energy. Needs a synthetic-shuffle-adjacent-to-speech case added to the
  voice-isolation selftest signal alongside the existing adjacent thump.
- **Workaround today:** the manual **Silence selection** / **Delete selection**
  operations (added in 0.17.0) exist precisely for the events no per-frame
  spectral feature can separate from voiced speech. Measurement on a real take
  (`bumps.wav`) confirmed these particular events are inseparable by any such
  feature, so the manual path is the honest answer for the worst cases even
  after the override is widened.

### The heard-position selftest fails intermittently (~1 run in 6)

- **Where:** `engine.h`, `RenderTimeline::sourceAt`, together with
  `EngineImpl::devicePlayed` in `engine.cpp`. Surfaces as
  `[FAIL] playback position never stalls between ticks` in `--selftest`.
- **Symptom:** the check asserts every sampling tick advances by at least 0.8× its
  own duration. It usually reports ≈0.999×, but roughly one run in six reports
  **0.60–0.65×** — one tick where the position barely moved. The companion
  "keeps real time" check always passes (≈0.97×), so this is a momentary
  plateau-then-jump, not drift. Reproduce by running `--selftest` six to ten
  times in a row.
- **Not** caused by the timeline selection overlay or anything else in the paint
  path: none of that code executes headlessly. It predates those changes.
- **Cause (analysis, not yet instrumented):** `devicePlayed` extrapolates the
  last published `IAudioClock` sample forward with QPC, capped at **100 ms**
  ahead. `sourceAt` then clamps: `if (devPlayed >= newest.devEnd) return
  newest.srcEnd`. With a 30 ms device buffer the audio thread republishes the
  clock only every ~30 ms, so a 100 ms extrapolation can *predict* the device
  past everything written and hit that clamp even though the buffer has not
  actually drained — the position plateaus until the next chunk is pushed, then
  jumps. The selftest samples at ~31 ms (`Sleep(20)` at the default timer
  resolution), which is nearly resonant with the 30 ms buffer period, so it
  drifts in and out of the clamp window — hence *intermittent* rather than
  constant. In the app the UI timer polls at 15 ms, twice per buffer period,
  which is why the end-to-end measurement came out uniform (15.3–15.7 ms of
  audio per screen refresh) and no stutter is visible in practice.
- **Proper fix:** tie the extrapolation cap to the actual clock-publish interval
  / device period instead of a flat 100 ms, so the prediction can never run past
  the newest recorded chunk unless the buffer genuinely drained. Distinguishing a
  *real* underrun (where freezing the playhead is the honest answer) from an
  over-eager prediction needs the buffer level (`GetCurrentPadding`) recorded
  alongside each `push`, so `sourceAt` can tell the two apart.
- **Do not** "fix" this by making the selftest sample at a non-resonant interval.
  The test is catching a real condition; hiding it would only make the plateau
  invisible.

## Technical debt

_(none currently tracked)_
