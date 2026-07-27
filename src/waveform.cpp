#include "waveform.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace wf {

// Signed perceptual companding: sqrt magnitude (so quiet parts stay visible)
// while preserving sign, so the min/max envelope keeps its true shape.
static inline float companded(float v) {
    float a = std::sqrt(std::min(1.0f, std::fabs(v)));
    return v < 0.0f ? -a : a;
}

void draw(HDC hdc, const RECT& rc, const AudioBuffer& buf, const PeakCache& pc,
          int64_t f0, int64_t f1, COLORREF color) {
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0 || f1 <= f0) return;

    const int64_t nf = buf.frames();
    if (nf > 0) { f0 = std::max<int64_t>(0, f0); f1 = std::min<int64_t>(nf, f1); }
    if (f1 <= f0) return;

    const int cy = rc.top + H / 2;
    const int halfH = H / 2 - 1;
    const double framesPerPx = (double)(f1 - f0) / W;
    const int ch = buf.channels > 0 ? buf.channels : 1;
    const float invCh = 1.0f / (float)ch;
    const float* s = buf.samples.data();
    const bool haveSamples = !buf.samples.empty() && nf > 0;

    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);

    auto monoAt = [&](int64_t f) -> float {
        const float* fr = s + f * ch;
        float m = 0.0f;
        for (int c = 0; c < ch; ++c) m += fr[c];
        return m * invCh;
    };

    // Sub-sample zoom: fewer frames than pixels -> draw the actual sample line.
    if (haveSamples && (f1 - f0) <= (int64_t)W) {
        std::vector<POINT> line;
        line.reserve((size_t)(f1 - f0) + 2);
        double pxPerFrame = (double)W / (double)(f1 - f0);
        for (int64_t f = f0; f <= f1 && f < nf; ++f) {
            int x = rc.left + (int)((double)(f - f0) * pxPerFrame);
            int y = cy - (int)(companded(monoAt(f)) * halfH);
            line.push_back({ x, y });
        }
        // baseline so a flat trace still shows the centre
        MoveToEx(hdc, rc.left, cy, nullptr); LineTo(hdc, rc.right, cy);
        if (line.size() >= 2) Polyline(hdc, line.data(), (int)line.size());
        SelectObject(hdc, op); SelectObject(hdc, ob);
        DeleteObject(pen); DeleteObject(br);
        return;
    }

    // Otherwise a filled min/max envelope: use the raw samples when zoomed in
    // enough that the bucket cache would look blocky, else the cache for speed.
    const bool useRaw = haveSamples && framesPerPx < (double)pc.bucketFrames;

    // One two-point polyline per pixel column, issued as a single PolyPolyline.
    //
    // This used to be a single Polygon tracing the max edge left-to-right and the
    // min edge back again. That looks the same but is enormously more expensive:
    // GDI has to scan-convert a ~3000-vertex, wildly self-overlapping outline,
    // intersecting every edge with every scanline. Measured on a 20 s clip across
    // 1478 px it cost 11 ms per call -- and the editor makes two calls per paint
    // (waveform, then the selection-coloured overlay), which put a floor of ~30 ms
    // under the whole redraw and was what actually made the playhead look jumpy.
    // Stroking vertical segments is trivial by comparison and, because each column
    // is drawn independently, it also can't produce the tangles the polygon did
    // where the two edges crossed.
    std::vector<POINT> pts;
    std::vector<DWORD> counts;
    pts.reserve((size_t)W * 2);
    counts.assign((size_t)W, 2);

    auto colMinMax = [&](int64_t a, int64_t b, float& lo, float& hi) {
        if (useRaw) {
            if (b > nf) b = nf;
            if (b <= a) { lo = hi = (a < nf ? monoAt(a) : 0.0f); return; }
            float mn = 1e30f, mx = -1e30f;
            for (int64_t f = a; f < b; ++f) { float m = monoAt(f); mn = std::min(mn, m); mx = std::max(mx, m); }
            lo = mn; hi = mx;
        } else {
            pc.rangeMinMax(a, b, lo, hi);
        }
    };

    for (int x = 0; x < W; ++x) {
        int64_t a = f0 + (int64_t)(x * framesPerPx);
        int64_t b = f0 + (int64_t)((x + 1) * framesPerPx);
        if (b <= a) b = a + 1;
        float lo, hi; colMinMax(a, b, lo, hi);
        int yTop = cy - (int)(companded(hi) * halfH);
        int yBot = cy - (int)(companded(lo) * halfH);
        // Polyline stops one short of its final point, so a silent column would
        // otherwise vanish entirely instead of leaving the centre line.
        if (yBot <= yTop) yBot = yTop + 1;
        pts.push_back({ rc.left + x, yTop });
        pts.push_back({ rc.left + x, yBot });
    }

    PolyPolyline(hdc, pts.data(), counts.data(), (DWORD)counts.size());
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen); DeleteObject(br);
}

} // namespace wf
