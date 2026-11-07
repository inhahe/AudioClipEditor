#include "match.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace match {

// ---------------------------------------------------------------- ClipSet

int ClipSet::changeCount() const {
    int n = 0; for (uint8_t c : change) n += c ? 1 : 0; return n;
}
int ClipSet::poolCount() const {
    int n = 0; for (uint8_t c : pool) n += c ? 1 : 0; return n;
}
int ClipSet::firstChanged() const {
    for (int i = 0; i < size(); ++i) if (change[i]) return i;
    return -1;
}
int ClipSet::indexOf(int clipId) const {
    for (int i = 0; i < size(); ++i) if (ids[i] == clipId) return i;
    return -1;
}
bool ClipSet::usable() const {
    if (changeCount() == 0) return false;
    return single() || size() >= 2;
}
std::vector<int> ClipSet::poolIndices() const {
    std::vector<int> v;
    for (int i = 0; i < size(); ++i) if (pool[i]) v.push_back(i);
    return v;
}
int ClipSet::referenceFor(int clipId) const {
    const int i = indexOf(clipId);
    return (i >= 0 && pool[i]) ? i : -1;
}

namespace {

std::wstring quoted(const std::wstring& s) { return L"'" + s + L"'"; }

std::wstring clipCount(int n) {
    return std::to_wstring(n) + (n == 1 ? L" clip" : L" clips");
}

// Append `c` if it has audio and isn't in the set yet. A clip placed twice on a
// track is still one clip with one volume and one tone colour; counting it twice
// would give it a double say in the average.
void add(ClipSet& s, const Clip* c, bool change, bool pool) {
    if (!c || !c->buffer || c->frames() <= 0) return;
    if (s.indexOf(c->id) >= 0) return;
    s.ids.push_back(c->id);
    s.names.push_back(c->name);
    s.change.push_back(change ? 1 : 0);
    s.pool.push_back(pool ? 1 : 0);
}

void finishSet(ClipSet& s, const std::wstring& what) {
    s.what = what;
    s.label = what + L" (" + clipCount(s.size()) + L")";
}

// One clip to the rest: `clipId` first (it is the subject of the command), then
// the pool as `others` lists it, minus that clip.
ClipSet oneToRest(const Project& p, int clipId, const std::vector<const Clip*>& others,
                  const std::wstring& where) {
    ClipSet s;
    const Clip* subject = p.findClip(clipId);
    add(s, subject, true, false);
    if (s.size() == 0) return s;   // no audio: nothing to match
    for (const Clip* c : others)
        if (c && c->id != clipId) add(s, c, false, true);
    const int n = s.poolCount();
    s.what = quoted(subject->name);
    s.others = (n == 1 ? std::wstring(L"the other clip") : L"the other " + clipCount(n)) + where;
    s.label = quoted(subject->name) + L" only, matched to " + s.others;
    return s;
}

} // namespace

ClipSet trackSet(const Project& p, int trackId) {
    ClipSet s;
    const Track* t = p.findTrack(trackId);
    if (!t) return s;
    for (const PlacedClip& pc : t->clips) add(s, p.findClip(pc.clipId), true, true);
    finishSet(s, L"track " + quoted(t->name));
    return s;
}

ClipSet timelineSet(const Project& p) {
    ClipSet s;
    for (const Track& t : p.tracks)
        for (const PlacedClip& pc : t.clips) add(s, p.findClip(pc.clipId), true, true);
    finishSet(s, L"the clips on all tracks");
    return s;
}

ClipSet librarySet(const Project& p) {
    ClipSet s;
    for (const Clip& c : p.library) add(s, &c, true, true);
    finishSet(s, L"all clips");
    return s;
}

ClipSet clipToTrack(const Project& p, int clipId, int trackId) {
    const Track* t = p.findTrack(trackId);
    if (!t) return ClipSet{};
    std::vector<const Clip*> others;
    for (const PlacedClip& pc : t->clips) others.push_back(p.findClip(pc.clipId));
    return oneToRest(p, clipId, others, L" on " + quoted(t->name));
}

ClipSet clipToLibrary(const Project& p, int clipId) {
    std::vector<const Clip*> others;
    for (const Clip& c : p.library) others.push_back(&c);
    return oneToRest(p, clipId, others, L"");
}

// ---------------------------------------------------------------- volume

namespace {

double toDb(double linear) { return 20.0 * std::log10(linear); }

double peakOf(const AudioBuffer& b) {
    float m = 0.0f;
    for (float v : b.samples) m = std::max(m, std::fabs(v));
    return m;
}

// The two clips furthest apart in level among those that were measured; invalid
// with fewer than two.
Spread levelSpread(const std::vector<double>& db, const std::vector<uint8_t>& measured) {
    Spread s;
    int hi = -1, lo = -1, count = 0;
    for (int i = 0; i < (int)db.size(); ++i) {
        if (!measured[i]) continue;
        ++count;
        if (hi < 0 || db[i] > db[hi]) hi = i;
        if (lo < 0 || db[i] < db[lo]) lo = i;
    }
    if (count < 2) return s;
    // All exactly level -- the outcome a match is aiming for -- leaves the loudest
    // and the quietest the same clip. Any other clip is then as far from it as any.
    if (hi == lo)
        for (int i = 0; i < (int)db.size(); ++i)
            if (measured[i] && i != hi) { lo = i; break; }
    s.db = db[hi] - db[lo]; s.a = hi; s.b = lo;
    return s;
}

} // namespace

VolumeResult matchVolume(const Project& p, const ClipSet& s, int reference) {
    VolumeResult r;
    const int n = s.size();
    r.measured.assign(n, 0);
    r.levelDb.assign(n, 0.0);
    r.oldGain.assign(n, 1.0f);
    r.newGain.assign(n, 1.0f);
    std::vector<double> loud(n, 0.0), peak(n, 0.0);
    for (int i = 0; i < n; ++i) {
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        r.oldGain[i] = r.newGain[i] = c->gain;
        // Below -100 dB FS the clip is effectively silent: the measurement is of
        // the noise floor, and matching that is meaningless.
        const double L = dsp::speechLoudness(*c->buffer);
        if (L < 1e-5 || c->gain <= 0.0f) {
            // A clip turned all the way down is silent too; there is no level of
            // it to compare with, and dividing by it would be a division by zero.
            ++r.unmeasured;
            continue;
        }
        r.measured[i] = 1;
        loud[i] = L;
        peak[i] = peakOf(*c->buffer);
        r.levelDb[i] = toDb(L * c->gain);
    }

    // The target, in dB at the clips' current volumes.
    if (reference >= 0) {
        if (reference >= n || !s.pool[reference] || !r.measured[reference]) return r;
        r.targetDb = r.levelDb[reference];
    } else {
        // Geometric mean of the pool's levels = arithmetic mean in dB.
        double acc = 0.0; int cnt = 0;
        for (int i = 0; i < n; ++i)
            if (s.pool[i] && r.measured[i]) { acc += r.levelDb[i]; ++cnt; }
        if (cnt == 0) return r;
        r.targetDb = acc / cnt;
    }
    r.ok = true;

    const double target = std::pow(10.0, r.targetDb / 20.0);
    for (int i = 0; i < n; ++i) {
        if (!s.change[i] || !r.measured[i] || i == reference) continue;
        const double want = target / loud[i];
        float g = (float)std::max<double>(kMinMatchGain, std::min<double>(kMaxMatchGain, want));
        // Held: the clamp moved it by more than float rounding would.
        if (std::fabs(toDb(g) - toDb(want)) > 0.01) r.held.push_back(i);
        // Under 0.01 dB is rounding, not a correction. Keeping the volume it has
        // means matching clips that already match changes nothing, and so leaves no
        // do-nothing step in the history.
        if (std::fabs(toDb(g) - toDb(r.oldGain[i])) < 0.01) g = r.oldGain[i];
        r.newGain[i] = g;
        if (g != r.oldGain[i]) {
            r.gains.push_back({ s.ids[i], g });
            ++r.changed;
            const double moved = toDb(g) - toDb(r.oldGain[i]);
            if (r.biggest < 0 || std::fabs(moved) > std::fabs(r.biggestDb)) {
                r.biggest = i; r.biggestDb = moved;
            }
        }
    }

    // How far apart they were and are now.
    std::vector<double> after(n, 0.0);
    for (int i = 0; i < n; ++i)
        if (r.measured[i]) after[i] = toDb(loud[i] * r.newGain[i]);
    if (s.single()) {
        const int i = s.firstChanged();
        if (r.measured[i]) {
            r.before = { std::fabs(r.levelDb[i] - r.targetDb), i, reference };
            r.after  = { std::fabs(after[i] - r.targetDb), i, reference };
        }
    } else {
        r.before = levelSpread(r.levelDb, r.measured);
        r.after  = levelSpread(after, r.measured);
    }

    // Track volume multiplies clip volume in the mix (Document::renderMix and the
    // engine's TimelineSource both apply clip gain x track gain, then clamp). So a
    // clip's loudest track decides whether it clips, and differing track volumes
    // undo a match in the mix. Muted tracks aren't in the mix, so they don't count.
    std::vector<float> trackMax(n, 0.0f);
    std::vector<uint8_t> placed(n, 0);
    for (const Track& t : p.tracks) {
        if (t.muted) continue;
        bool holds = false;
        for (const PlacedClip& pc : t.clips) {
            const int i = s.indexOf(pc.clipId);
            if (i < 0) continue;
            holds = true;
            trackMax[i] = placed[i] ? std::max(trackMax[i], t.gain) : t.gain;
            placed[i] = 1;
        }
        if (holds) r.trackGains.push_back({ t.name, t.gain });
    }
    // Only worth a word when they differ audibly; 0.1 dB apart is the same volume.
    bool differ = false;
    for (const auto& a : r.trackGains)
        for (const auto& b : r.trackGains) {
            const float x = a.second, y = b.second;
            if (x <= 0.0f || y <= 0.0f ? x != y : std::fabs(toDb(x) - toDb(y)) > 0.1) differ = true;
        }
    if (!differ) r.trackGains.clear();

    for (int i = 0; i < n; ++i) {
        if (!s.change[i] || !r.measured[i]) continue;
        // Unplaced, it is only ever heard on its own card, at its own volume.
        const double level = peak[i] * r.newGain[i] * (placed[i] ? trackMax[i] : 1.0f);
        if (level > 1.0) r.overFullScale.push_back({ i, toDb(level) });
    }
    std::sort(r.overFullScale.begin(), r.overFullScale.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return r;
}

// ---------------------------------------------------------------- timbre

namespace {

Spread worstPair(const std::vector<dsp::TimbreProfile>& ps) {
    Spread w;
    for (int i = 0; i < (int)ps.size(); ++i)
        for (int j = i + 1; j < (int)ps.size(); ++j) {
            // Unmeasured or mismatched profiles return -1 and so can never win.
            const double d = dsp::timbreDistanceDb(ps[i], ps[j]);
            if (d > w.db) { w.db = d; w.a = i; w.b = j; }
        }
    return w;
}

} // namespace

TimbreResult matchTimbre(const Project& p, const ClipSet& s, int reference,
                         const dsp::TimbreMatchOptions& opts) {
    TimbreResult r;
    const int n = s.size();
    // Bin-for-bin comparison of spectra only means anything at one rate. The first
    // matched clip sets it -- in "one clip to the rest" that is the clip being
    // matched, which must not be the one left out -- and clips at any other rate are
    // left out and counted, since a clip missing from a match is exactly the one
    // that will still sound different.
    const int first = s.firstChanged();
    if (first < 0) return r;
    if (const Clip* c = p.findClip(s.ids[first]); c && c->buffer) r.rate = c->buffer->sampleRate;

    // Measure everything before filtering anything: the target may be the average
    // of these, and a clip that can't be measured must be left alone rather than
    // filtered by a curve derived from nothing.
    std::vector<dsp::TimbreProfile> profiles((size_t)n);
    for (int i = 0; i < n; ++i) {
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        if (c->buffer->sampleRate != r.rate) { ++r.mismatched; continue; }
        profiles[i] = dsp::computeTimbreProfile(*c->buffer);
        if (!profiles[i].valid()) ++r.unmeasured;
    }

    dsp::TimbreProfile target;
    if (reference >= 0) {
        if (reference >= n || !s.pool[reference]) return r;
        target = profiles[reference];
    } else {
        std::vector<dsp::TimbreProfile> poolProfiles;
        for (int i = 0; i < n; ++i) if (s.pool[i]) poolProfiles.push_back(profiles[i]);
        target = dsp::averageTimbre(poolProfiles);
    }
    if (!target.valid()) return r;
    r.ok = true;

    std::vector<dsp::TimbreProfile> after = profiles;
    const double binHz = dsp::timbreBinHz(r.rate);
    for (int i = 0; i < n; ++i) {
        // The reference is the standard: it is left exactly as it is, not run
        // through an identity filter that would cost a pass and could only add
        // rounding to the one clip the user said was right.
        if (!s.change[i] || i == reference || !profiles[i].valid()) continue;
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        std::vector<float> curve;
        AudioBufferPtr matched = dsp::matchTimbre(*c->buffer, target, opts, &curve);
        if (!matched) continue;
        // Where the biggest move landed, not just how big it was: 8 dB at 250 Hz is
        // a mic-distance difference, 8 dB at 40 Hz is a fan.
        for (size_t k = 0; k < curve.size(); ++k)
            if (std::fabs(curve[k]) > r.worstDb) {
                r.worstDb = std::fabs(curve[k]);
                r.worstHz = (double)k * binHz;
                r.worstClip = i;
            }
        r.updates.push_back({ s.ids[i], matched });
        // Re-measure the result. The size of the correction says how far a clip was
        // moved, which is a different question from how close it ended up: a big
        // correction that landed is a success, and a small one that was clamped is a
        // failure. Without the second number, telling "the EQ stopped short" from
        // "the difference isn't spectral at all" is guesswork.
        after[i] = dsp::computeTimbreProfile(*matched);
    }

    if (s.single()) {
        const int i = first;
        const double b = dsp::timbreDistanceDb(profiles[i], target);
        const double a = dsp::timbreDistanceDb(after[i], target);
        if (b >= 0.0 && a >= 0.0) { r.before = { b, i, reference }; r.after = { a, i, reference }; }
    } else {
        r.before = worstPair(profiles);
        r.after = worstPair(after);
    }
    return r;
}

} // namespace match
