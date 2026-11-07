#include "match.h"
#include <algorithm>
#include <cmath>

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

// m:ss.s, for saying where on the timeline a clip sits.
std::wstring timeText(int64_t frames, int rate) {
    const double sec = rate > 0 ? (double)frames / rate : 0.0;
    wchar_t b[32];
    swprintf(b, 32, L"%d:%04.1f", (int)(sec / 60.0), sec - 60.0 * (int)(sec / 60.0));
    return b;
}

// Clips that share a name can't be told apart in a list of names -- "Sound like
// 'take'" twice over -- or in a summary that names them. So where a name repeats
// within the set, each of those clips says where it is: its first place on the
// timeline, or failing that its place in the library.
void disambiguate(const Project& p, ClipSet& s) {
    const std::vector<std::wstring> plain = s.names;
    for (int i = 0; i < s.size(); ++i) {
        if (std::count(plain.begin(), plain.end(), plain[(size_t)i]) < 2) continue;
        std::wstring where;
        for (const Track& t : p.tracks) {
            for (const PlacedClip& pc : t.clips)   // sorted by start: the first is the earliest
                if (pc.clipId == s.ids[(size_t)i]) { where = t.name + L", " + timeText(pc.startFrame, p.sampleRate); break; }
            if (!where.empty()) break;
        }
        if (where.empty())
            for (size_t k = 0; k < p.library.size(); ++k)
                if (p.library[k].id == s.ids[(size_t)i]) { where = L"library #" + std::to_wstring(k + 1); break; }
        s.names[(size_t)i] = plain[(size_t)i] + L" (" + where + L")";
    }
}

void finishSet(const Project& p, ClipSet& s, const std::wstring& what) {
    disambiguate(p, s);
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
    disambiguate(p, s);
    const int n = s.poolCount();
    s.what = quoted(s.names[0]);
    s.others = (n == 1 ? std::wstring(L"the other clip") : L"the other " + clipCount(n)) + where;
    s.label = quoted(s.names[0]) + L" only, matched to " + s.others;
    return s;
}

} // namespace

namespace {

// The tracks of `ids`, in the project's order (not the list's), without repeats.
std::vector<const Track*> tracksOf(const Project& p, const std::vector<int>& ids) {
    std::vector<const Track*> out;
    for (const Track& t : p.tracks)
        if (std::find(ids.begin(), ids.end(), t.id) != ids.end()) out.push_back(&t);
    return out;
}

// "track 'Narration'", "tracks 'Narration' and 'Pickups'", "4 tracks" -- or, when
// every track that has clips is included, "all tracks".
std::wstring tracksText(const Project& p, const std::vector<const Track*>& ts) {
    int withClips = 0, chosenWithClips = 0;
    for (const Track& t : p.tracks) withClips += t.clips.empty() ? 0 : 1;
    for (const Track* t : ts) chosenWithClips += t->clips.empty() ? 0 : 1;
    if (ts.size() == 1) return L"track " + quoted(ts[0]->name);
    if (withClips > 1 && chosenWithClips == withClips) return L"all tracks";
    if (ts.size() == 2) return L"tracks " + quoted(ts[0]->name) + L" and " + quoted(ts[1]->name);
    return std::to_wstring(ts.size()) + L" tracks";
}

} // namespace

ClipSet tracksSet(const Project& p, const std::vector<int>& trackIds) {
    ClipSet s;
    const std::vector<const Track*> ts = tracksOf(p, trackIds);
    if (ts.empty()) return s;
    for (const Track* t : ts)
        for (const PlacedClip& pc : t->clips) add(s, p.findClip(pc.clipId), true, true);
    const std::wstring where = tracksText(p, ts);
    finishSet(p, s, where == L"all tracks" ? std::wstring(L"the clips on all tracks")
                  : where.rfind(L"track ", 0) == 0 ? where : L"the clips on " + where);
    return s;
}

ClipSet trackSet(const Project& p, int trackId) { return tracksSet(p, { trackId }); }

ClipSet timelineSet(const Project& p) {
    std::vector<int> all;
    for (const Track& t : p.tracks) all.push_back(t.id);
    return tracksSet(p, all);
}

ClipSet librarySet(const Project& p) {
    ClipSet s;
    for (const Clip& c : p.library) add(s, &c, true, true);
    finishSet(p, s, L"all clips");
    return s;
}

ClipSet clipToTracks(const Project& p, int clipId, const std::vector<int>& trackIds) {
    const std::vector<const Track*> ts = tracksOf(p, trackIds);
    if (ts.empty()) return ClipSet{};
    std::vector<const Clip*> others;
    for (const Track* t : ts)
        for (const PlacedClip& pc : t->clips) others.push_back(p.findClip(pc.clipId));
    const std::wstring where = tracksText(p, ts);
    return oneToRest(p, clipId, others,
                     where.rfind(L"track ", 0) == 0 ? L" on " + quoted(ts[0]->name) : L" on " + where);
}

ClipSet clipToTrack(const Project& p, int clipId, int trackId) {
    return clipToTracks(p, clipId, { trackId });
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
    std::vector<double> loud(n, 0.0);
    for (int i = 0; i < n; ++i) {
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        r.oldGain[i] = r.newGain[i] = c->gain;
        // Below -100 dB FS the clip is effectively silent: the measurement is of
        // the noise floor, and matching that is meaningless.
        const double L = dsp::speechLoudness(*c->heard());   // as it sounds, EQ and all
        if (L < 1e-5 || c->gain <= 0.0f) {
            // A clip turned all the way down is silent too; there is no level of
            // it to compare with, and dividing by it would be a division by zero.
            ++r.unmeasured;
            continue;
        }
        r.measured[i] = 1;
        loud[i] = L;
        r.levelDb[i] = toDb(L * c->gain);
    }

    // The target, in dB at the clips' current volumes -- and if there is none, why.
    int measuredCount = 0;
    for (uint8_t m : r.measured) measuredCount += m ? 1 : 0;
    if (s.single() && !r.measured[s.firstChanged()]) { r.failure = Failure::Subject; return r; }
    if (reference >= 0) {
        if (reference >= n || !s.pool[reference] || !r.measured[reference]) {
            r.failure = Failure::Reference; return r;
        }
        r.targetDb = r.levelDb[reference];
    } else {
        // Geometric mean of the pool's levels = arithmetic mean in dB.
        double acc = 0.0; int cnt = 0;
        for (int i = 0; i < n; ++i)
            if (s.pool[i] && r.measured[i]) { acc += r.levelDb[i]; ++cnt; }
        if (cnt == 0) { r.failure = Failure::Pool; return r; }
        r.targetDb = acc / cnt;
    }
    // A set needs two clips that can be compared; with one, "they already match"
    // would be a claim about clips that were never measured.
    if (!s.single() && measuredCount < 2) { r.failure = Failure::Alone; return r; }

    const double target = std::pow(10.0, r.targetDb / 20.0);
    for (int i = 0; i < n; ++i) {
        if (!s.change[i] || !r.measured[i] || i == reference) continue;
        const double want = target / loud[i];
        // The limits never push a clip outward: one already set below -26 dB by hand
        // may stay where it is (or come up towards the target), but is not raised to
        // -26 dB when that is further from the target than it is now.
        const double lo = std::min<double>(kMinMatchGain, r.oldGain[i]);
        float g = (float)std::max<double>(lo, std::min<double>(kMaxMatchGain, want));
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

    // A clip is heard two ways: on its own (its card, and Export clip) at its own
    // volume, and in the mix at its volume times its loudest track's. Either one
    // going over full scale clips. Only clips this match turned up are looked at --
    // it did not make the others clip -- which also keeps the scan of every sample
    // to the few clips that need it.
    for (int i = 0; i < n; ++i) {
        if (!s.change[i] || !r.measured[i] || !(r.newGain[i] > r.oldGain[i])) continue;
        const Clip* c = p.findClip(s.ids[i]);
        const double alone = peakOf(*c->heard()) * r.newGain[i];
        const double mix = placed[i] ? alone * trackMax[i] : 0.0;
        if (alone > 1.0 || mix > 1.0)
            r.overFullScale.push_back({ i, toDb(std::max(alone, mix)), mix > 1.0 });
    }
    std::sort(r.overFullScale.begin(), r.overFullScale.end(),
              [](const OverFullScale& a, const OverFullScale& b) { return a.db > b.db; });
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
    const int first = s.firstChanged();
    if (first < 0) { r.failure = Failure::Subject; return r; }
    r.measured.assign(n, 0);
    r.otherRate.assign(n, 0);
    // Bin-for-bin comparison of spectra only means anything at one rate, so clips at
    // any other rate are left out and counted -- a clip missing from a match is
    // exactly the one that will still sound different. In one clip to the rest the
    // clip being matched sets the rate, since it must not be the one left out. In a
    // set it is the rate most clips share (the earliest on a tie): taking the first
    // clip's would let one odd clip at the head of a track exclude all the others.
    std::vector<int> rates((size_t)n, 0);
    for (int i = 0; i < n; ++i)
        if (const Clip* c = p.findClip(s.ids[i]); c && c->buffer) rates[i] = c->buffer->sampleRate;
    r.rate = rates[first];
    if (!s.single()) {
        int best = 0;
        for (int i = 0; i < n; ++i) {
            const int votes = (int)std::count(rates.begin(), rates.end(), rates[i]);
            if (votes > best) { best = votes; r.rate = rates[i]; }
        }
    }

    // Measure everything before filtering anything: the target may be the average
    // of these, and a clip that can't be measured must be left alone rather than
    // filtered by a curve derived from nothing. Each clip is measured as it is
    // *heard*, its current EQ included -- that is the tone colour the others are
    // matched to, and the one "before" describes.
    std::vector<dsp::TimbreProfile> profiles((size_t)n);
    for (int i = 0; i < n; ++i) {
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        if (rates[i] != r.rate) { ++r.mismatched; r.otherRate[i] = 1; continue; }
        profiles[i] = dsp::computeTimbreProfile(*c->heard());
        if (profiles[i].valid()) r.measured[i] = 1;
        else ++r.unmeasured;
    }

    // The target -- and if there is none, why.
    if (s.single() && !r.measured[first]) { r.failure = Failure::Subject; return r; }
    dsp::TimbreProfile target;
    if (reference >= 0) {
        if (reference >= n || !s.pool[reference] || !r.measured[reference]) {
            r.failure = Failure::Reference; return r;
        }
        target = profiles[reference];
    } else {
        std::vector<dsp::TimbreProfile> poolProfiles;
        for (int i = 0; i < n; ++i) if (s.pool[i]) poolProfiles.push_back(profiles[i]);
        target = dsp::averageTimbre(poolProfiles);
        if (!target.valid()) { r.failure = Failure::Pool; return r; }
    }
    // A set needs two measured clips; with one, the "match" would filter that clip
    // towards its own spectrum and rewrite its audio for nothing.
    if (!s.single() && std::count(r.measured.begin(), r.measured.end(), 1) < 2) {
        r.failure = Failure::Alone; return r;
    }

    std::vector<dsp::TimbreProfile> after = profiles;
    const double binHz = dsp::timbreBinHz(r.rate);
    for (int i = 0; i < n; ++i) {
        // The reference is the standard: it is left exactly as it is, not run
        // through an identity filter that would cost a pass and could only add
        // rounding to the one clip the user said was right.
        if (!s.change[i] || i == reference || !profiles[i].valid()) continue;
        const Clip* c = p.findClip(s.ids[i]);
        if (!c || !c->buffer) continue;
        // The new EQ is designed from the clip's own audio, *not* from what it sounds
        // like through its current EQ: it replaces that EQ rather than stacking on
        // it, so matching again lands in the same place instead of compounding. It
        // keeps the loudness the clip is heard at now.
        const dsp::TimbreEq eq = dsp::designTimbreEq(*c->buffer, target, opts,
                                                     dsp::speechLoudness(*c->heard()));
        if (!eq.result) continue;
        // Where the biggest move landed, not just how big it was: 8 dB at 250 Hz is
        // a mic-distance difference, 8 dB at 40 Hz is a fan.
        for (size_t k = 0; k < eq.shapeDb.size(); ++k)
            if (std::fabs(eq.shapeDb[k]) > r.worstDb) {
                r.worstDb = std::fabs(eq.shapeDb[k]);
                r.worstHz = (double)k * binHz;
                r.worstClip = i;
            }
        r.updates.push_back({ s.ids[i], std::make_shared<const std::vector<float>>(eq.curveDb),
                              eq.result });
        const AudioBufferPtr& matched = eq.result;
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
