#pragma once
// A volume as text: the readouts beside the sliders, the menus that name a volume,
// and the box a volume is typed into. Pure, so the selftest reaches it.
#include "model.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

namespace vol {

// "150%". Whole percents, which is what a slider's readout has room for and finer
// than anyone hears -- except below 10%, where one percent is a big step (2% to 3%
// is 3.5 dB), so one decimal is kept there. A volume too small for that still plays,
// so it reads "<0.1%": only silence reads 0%.
inline std::wstring percent(float gain) {
    const double p = std::max(0.0, (double)gain * 100.0);
    if (p > 0.0 && p < 0.05) return L"<0.1%";
    wchar_t b[32];
    swprintf(b, 32, p > 0.0 && p < 9.95 ? L"%.1f%%" : L"%.0f%%", p);
    return b;
}

// The same volume as a change in decibels: "+3.5 dB", "-6.0 dB", "0.0 dB" at 100%,
// and "silent" at 0%, which no number of decibels reaches.
inline std::wstring decibels(float gain) {
    if (!(gain > 0.0f)) return L"silent";
    double db = 20.0 * std::log10((double)gain);
    if (std::fabs(db) < 0.05) db = 0.0;
    wchar_t b[32];
    swprintf(b, 32, db == 0.0 ? L"%.1f dB" : L"%+.1f dB", db);
    return b;
}

// What parse() made of a typed volume.
struct Parsed {
    bool ok = false;
    float gain = 1.0f;      // linear, 0 .. kMaxClipGain, when ok
    std::wstring why;       // when not ok: what is wrong, said as what to type instead
};

// Read a typed volume. Two units, because people think of a volume both ways:
//   "150%", "150 %", "150"          a percentage (a bare number is one, as the
//                                   readouts are in percent);
//   "+3.5 dB", "-6dB", "+3", "-6"    decibels -- a signed bare number too, since a
//                                   sign only means something as a change in dB;
//   "-inf dB"                        silent.
// The minus may be typed as a hyphen, a minus sign or an en dash, and a comma may
// stand for the decimal point (but see "1,600" below). Refused, rather than quietly
// clamped, past the limits of 0% and +24 dB: a typed number that silently became
// another is worse than one sent back.
inline Parsed parse(const std::wstring& text) {
    Parsed r;
    std::wstring s;
    for (wchar_t ch : text) {
        if (ch == L'\u2212' || ch == L'\u2013') ch = L'-';
        if (ch >= L'A' && ch <= L'Z') ch = (wchar_t)(ch - L'A' + L'a');
        s += ch;
    }
    auto trim = [](std::wstring& t) {
        const size_t a = t.find_first_not_of(L" \t");
        if (a == std::wstring::npos) { t.clear(); return; }
        t = t.substr(a, t.find_last_not_of(L" \t") - a + 1);
    };
    trim(s);
    const wchar_t* kHow = L"Type a percentage (150%) or decibels (+3.5 dB, -6 dB).";
    if (s.empty()) { r.why = kHow; return r; }

    enum class Unit { None, Percent, Db } unit = Unit::None;
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, L"db") == 0) { unit = Unit::Db; s.resize(s.size() - 2); }
    else if (s.back() == L'%') { unit = Unit::Percent; s.pop_back(); }
    trim(s);
    if (unit == Unit::None && !s.empty()) unit = (s[0] == L'+' || s[0] == L'-') ? Unit::Db : Unit::Percent;

    const double maxDb = 20.0 * std::log10((double)kMaxClipGain);
    // Said in whole dB, as everywhere else the limit is named: "+24 dB", not "+24.1".
    wchar_t maxDbText[16];
    swprintf(maxDbText, 16, L"%+.0f dB", maxDb);
    const std::wstring limit = L"The most a volume can be is " + percent(kMaxClipGain) +
                               L" (" + maxDbText + L").";
    if (unit == Unit::Db && (s == L"-inf" || s == L"-infinity" || s == L"-\u221E")) {
        r.ok = true; r.gain = 0.0f; return r;
    }
    if (s.find(L'.') == std::wstring::npos) {
        // A comma with exactly three digits after it is the one place the two readings
        // part: "1,600" is sixteen hundred to some and 1.6 to others, and either guess
        // is a volume 60 dB off for the rest. So that is asked about, not guessed --
        // unless what is before it is a lone 0, which no thousands figure starts with.
        const size_t comma = s.find(L',');
        if (comma != std::wstring::npos && comma + 4 == s.size()) {
            auto digit = [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; };
            const size_t from = (s[0] == L'+' || s[0] == L'-') ? 1 : 0;
            const std::wstring whole = s.substr(from, comma - from);
            const bool three = digit(s[comma + 1]) && digit(s[comma + 2]) && digit(s[comma + 3]);
            if (three && !whole.empty() && whole != L"0") {
                r.why = L"Is that comma a thousands separator or a decimal point? Type it "
                        L"without a comma (1600%), or with a point (1.6%).";
                return r;
            }
        }
        for (wchar_t& ch : s) if (ch == L',') ch = L'.';
    }
    wchar_t* end = nullptr;
    const double v = s.empty() ? 0.0 : wcstod(s.c_str(), &end);
    if (s.empty() || end == s.c_str() || *end != L'\0' || !std::isfinite(v)) { r.why = kHow; return r; }

    if (unit == Unit::Db) {
        // The readouts round to a tenth of a dB, so the limit as shown is accepted.
        if (v > maxDb + 0.05) { r.why = limit; return r; }
        r.gain = (float)std::pow(10.0, v / 20.0);
    } else {
        if (v < 0.0) { r.why = L"A volume can't be below 0% (silent). For a cut in decibels, type it with dB: -6 dB."; return r; }
        if (v > kMaxClipGain * 100.0 + 0.5) { r.why = limit; return r; }
        r.gain = (float)(v / 100.0);
    }
    r.gain = std::max(0.0f, std::min(kMaxClipGain, r.gain));
    r.ok = true;
    return r;
}

}  // namespace vol
