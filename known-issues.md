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

### Edit history does not survive save/load

- **Where:** `document.cpp` — `saveProject` (the `.acep` v3 writer stops at the
  view state) and `loadProject:438`, `undo_.init(project_)` — "fresh history for
  loaded project".
- **Symptom:** reopening a project gives you a History window with exactly one
  step and no undo. Saving is fine *within* a session (`markSaved` only moves the
  saved marker onto the current node; the tree is untouched), but quitting loses
  every step.
- **Why it matters:** the History window exists because a timbre match, a noise
  reduction or a normalise changes nothing you can see, so the list is the only
  way to answer "did I apply that, or did I undo it?". That answer evaporates on
  reload — which is precisely when it is most likely to be asked, since a session
  boundary is where memory of what was done runs out.
- **Cause / why it isn't just a serialisation chore:** every `UndoNode` holds a
  full `Project` snapshot. In memory that is cheap because the clips' buffers are
  shared via `shared_ptr`, but the `.acep` format writes each clip's raw float
  samples inline (`saveProject:357`) with no dedup or back-reference. Writing N
  history steps the obvious way writes the audio up to N times — a 20-step
  history on a 200 MB project would be a multi-gigabyte file.
- **Proper fix:** content-address the audio in the format. Bump to v4, write each
  *unique* `AudioBuffer` once into a buffer pool keyed by identity, have clips
  (in every snapshot) reference a pool index, then serialise the undo tree as
  nodes of `{parent, desc, lastChild, project-with-buffer-indices}`. Reload
  rebuilds the pool as `shared_ptr`s so the in-memory sharing is restored, which
  also makes the load cheaper than it is today. Worth capping the persisted depth
  (and offering "save without history"), since even deduped, a history that
  spans several destructive edits stores every intermediate version of the audio.
- **Workaround today:** none in-app. Before quitting, the History window is the
  record — read it while it still exists.
