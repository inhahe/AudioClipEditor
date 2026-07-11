#pragma once
#include <windows.h>
#include "audio_buffer.h"

namespace wf {

// Draw an oscilloscope/volume waveform of frames [f0,f1) into rc. Does not paint
// the background (caller fills rc first). color is the fill/line colour.
//
// The renderer adapts to zoom automatically:
//   - zoomed out  -> filled min/max envelope from the bucket cache (fast)
//   - zoomed in   -> per-pixel min/max scanned from the raw samples
//   - sub-sample  -> a connected line through the actual samples (true scope)
//
// buf provides the raw samples for the zoomed-in cases; pc is the precomputed
// bucket cache used when zoomed out. Either may be empty; the other is used.
void draw(HDC hdc, const RECT& rc, const AudioBuffer& buf, const PeakCache& pc,
          int64_t f0, int64_t f1, COLORREF color);

} // namespace wf
