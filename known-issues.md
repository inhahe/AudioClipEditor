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

### A loaded history holds all its audio in RAM

- **Where:** `document.cpp`, `loadProject` — the v4 trailer reader fills a
  `BufferPool` with every superseded `AudioBuffer` the history refers to, and
  `buildPeaks` runs over each one.
- **Symptom:** opening a project with a long history of *destructive* edits
  (timbre match, noise reduction, normalise) costs both memory and load time in
  proportion to the history, not to the project. The audio budget caps this at
  512 MB of superseded samples on top of the project itself, so it is bounded,
  but the worst case is a noticeably slower open and a much larger process.
- **Why it isn't urgent:** ordinary edits — placements, moves, gains, renames —
  add nothing at all, because their snapshots share the live library's buffers.
  Only edits that actually rewrite audio contribute, and only the versions no
  longer in use.
- **Proper fix:** load the trailer lazily. Record each pool slot's file offset
  and length instead of its samples, and fault the buffer in (plus its peaks) the
  first time a snapshot needing it is actually restored. `AudioBufferPtr` is
  already a `shared_ptr`, so the swap-in point is contained; the awkward part is
  that `loadProject` currently closes the file before returning, so the Document
  would have to keep a handle (or the path plus offsets) alive.
- **Workaround today:** `File ▸ Store the edit history in project files` off.

### History node count grows without bound across sessions

- **Where:** `undo.h` / the v4 trailer. The audio budget caps the *samples* a
  save may store, but nothing caps the number of steps.
- **Symptom:** a project worked on over months accumulates every step ever taken.
  Each name-only step is small (roughly a description plus three ints, ~100
  bytes), so 10 000 steps is on the order of 1 MB of file and a comparable amount
  of RAM — currently a non-problem, but monotonic.
- **Mitigated, not solved:** the stack-overflow consequence *is* fixed —
  `~UndoNode` tears the tree down iteratively, and a selftest builds and destroys
  a 20 000-step history (it kills the process without the fix). What remains is
  unbounded growth.
- **Proper fix:** cap the persisted step count by re-rooting. Walk up from the
  current node keeping at most N ancestors, drop everything above, and write the
  new topmost kept node as the root — its snapshot is self-contained, so the
  result is a valid tree that simply can't undo as far back. Needs a rule for
  side branches hanging off the dropped prefix (drop with it) and a decision on
  whether the dropped steps should survive as name-only entries, which would
  defeat the point unless the cap is on *snapshots* rather than on nodes.
