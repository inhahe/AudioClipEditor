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

## Technical debt

_(none currently tracked)_
