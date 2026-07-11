#include "waveform.h"
#include <vector>
#include <algorithm>
#include <cmath>

namespace wf {

void draw(HDC hdc, const RECT& rc, const PeakCache& pc,
          int64_t f0, int64_t f1, COLORREF color) {
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0 || f1 <= f0 || pc.peaks.empty()) return;

    const int cy = rc.top + H / 2;
    const int halfH = H / 2 - 1;
    const double framesPerPx = (double)(f1 - f0) / W;

    std::vector<POINT> pts;
    pts.reserve((size_t)W * 2 + 2);

    // top edge left->right, then bottom edge right->left (mirrored) => filled shape
    for (int x = 0; x < W; ++x) {
        int64_t a = f0 + (int64_t)(x * framesPerPx);
        int64_t b = f0 + (int64_t)((x + 1) * framesPerPx);
        if (b <= a) b = a + 1;
        float p = pc.peakInRange(a, b);
        // mild perceptual curve so quiet parts are visible
        float amp = std::sqrt(std::min(1.0f, std::max(0.0f, p)));
        int h = (int)(amp * halfH);
        pts.push_back({ rc.left + x, cy - h });
    }
    for (int x = W - 1; x >= 0; --x) {
        int64_t a = f0 + (int64_t)(x * framesPerPx);
        int64_t b = f0 + (int64_t)((x + 1) * framesPerPx);
        if (b <= a) b = a + 1;
        float p = pc.peakInRange(a, b);
        float amp = std::sqrt(std::min(1.0f, std::max(0.0f, p)));
        int h = (int)(amp * halfH);
        pts.push_back({ rc.left + x, cy + h });
    }

    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    Polygon(hdc, pts.data(), (int)pts.size());
    // center line
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

} // namespace wf
