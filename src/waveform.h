#pragma once
#include <windows.h>
#include "audio_buffer.h"

namespace wf {

// Draw a filled "volume graph" waveform of frames [f0,f1) from the peak cache into
// rc. Does not paint the background (caller fills rc first). color is the fill.
void draw(HDC hdc, const RECT& rc, const PeakCache& pc,
          int64_t f0, int64_t f1, COLORREF color);

} // namespace wf
