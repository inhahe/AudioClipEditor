#include "document.h"
#include "dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

// The timestamp used for "sort by time": the source file's last-write time when
// the clip came from disk, else the current wall-clock time (derived clips).
static uint64_t clipTimestampFor(const std::wstring& sourcePath) {
    if (!sourcePath.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(sourcePath.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            if (u.QuadPart) return u.QuadPart;
        }
    }
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void Document::init(int sampleRate) {
    project_ = Project{};
    view_ = ViewState{};
    project_.sampleRate = sampleRate;
    // start with a single track
    Track t;
    t.id = project_.nextTrackId++;
    t.name = L"Track 1";
    project_.tracks.push_back(std::move(t));
    undo_.init(project_);
    markSaved();
}

PeakCachePtr Document::buildPeaks(const AudioBufferPtr& buf) {
    auto pc = std::make_shared<PeakCache>();
    if (buf) pc->build(*buf);
    return pc;
}

int Document::newClipId() { return project_.nextClipId; }

int Document::addClip(const std::wstring& name, AudioBufferPtr buf,
                      const std::wstring& sourcePath, const std::wstring& undoDesc) {
    Clip c;
    c.id = project_.nextClipId++;
    c.name = name;
    c.sourcePath = sourcePath;
    c.buffer = buf;
    c.peaks = buildPeaks(buf);
    c.timestamp = clipTimestampFor(sourcePath);
    int id = c.id;
    project_.library.push_back(std::move(c));
    commit(undoDesc);
    return id;
}

void Document::renameClip(int id, const std::wstring& name) {
    Clip* c = project_.findClip(id);
    if (!c || c->name == name) return;
    c->name = name;
    commit(L"Rename to '" + name + L"'");
}

void clampSelection(const Project& project, int& clipId, int64_t& start, int64_t& end) {
    const Clip* c = clipId >= 0 ? project.findClip(clipId) : nullptr;
    if (c) {
        const int64_t nf = c->frames();
        start = std::max<int64_t>(0, std::min(start, nf));
        end   = std::max<int64_t>(0, std::min(end, nf));
        if (end > start) return;     // still a usable range — leave it alone
    }
    clipId = -1; start = end = 0;
}

void Document::removeClip(int id) {
    Clip* c = project_.findClip(id);
    if (!c) return;
    std::wstring nm = c->name;
    // remove placements of this clip from all tracks
    for (auto& t : project_.tracks) {
        auto& v = t.clips;
        v.erase(std::remove_if(v.begin(), v.end(),
            [&](const PlacedClip& p) { return p.clipId == id; }), v.end());
    }
    auto& lib = project_.library;
    lib.erase(std::remove_if(lib.begin(), lib.end(),
        [&](const Clip& x) { return x.id == id; }), lib.end());
    commit(L"Delete clip '" + nm + L"'");
}

int Document::replaceClipWithNew(int oldClipId, const std::wstring& name, AudioBufferPtr buf,
                                 const std::wstring& sourcePath, const std::wstring& undoDesc) {
    auto& lib = project_.library;
    int pos = -1;
    for (int i = 0; i < (int)lib.size(); ++i) if (lib[i].id == oldClipId) { pos = i; break; }
    if (pos < 0 || !buf) return -1;

    Clip c;
    c.id = project_.nextClipId++;
    c.name = name;
    c.sourcePath = sourcePath;
    c.buffer = std::move(buf);
    c.peaks = buildPeaks(c.buffer);
    // The replacement stands in for the old clip, so it plays at the level the old
    // one did; the samples are untouched, and gain is applied at playback.
    c.gain = lib[(size_t)pos].gain;
    c.timestamp = clipTimestampFor(sourcePath);
    const int id = c.id;

    for (auto& t : project_.tracks) {
        auto& v = t.clips;
        v.erase(std::remove_if(v.begin(), v.end(),
            [&](const PlacedClip& p) { return p.clipId == oldClipId; }), v.end());
    }
    lib[(size_t)pos] = std::move(c);
    commit(undoDesc);
    return id;
}

void Document::moveClipInLibrary(int clipId, int targetIndex) {
    auto& lib = project_.library;
    int from = -1;
    for (int i = 0; i < (int)lib.size(); ++i) if (lib[i].id == clipId) { from = i; break; }
    if (from < 0) return;
    if (targetIndex < 0) targetIndex = 0;
    if (targetIndex > (int)lib.size()) targetIndex = (int)lib.size();
    // Dropping just before or just after the current slot is a no-op.
    if (targetIndex == from || targetIndex == from + 1) return;
    Clip c = std::move(lib[from]);
    lib.erase(lib.begin() + from);
    if (targetIndex > from) --targetIndex;   // account for the removal above
    lib.insert(lib.begin() + targetIndex, std::move(c));
    commit(L"Reorder clips");
}

void Document::sortLibrary(bool byName) {
    auto& lib = project_.library;
    if (lib.size() < 2) return;
    std::vector<Clip> before = lib;  // detect an actual change to avoid a no-op undo step
    std::stable_sort(lib.begin(), lib.end(), [byName](const Clip& a, const Clip& b) {
        if (byName) {
            int cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
            if (cmp != 0) return cmp < 0;
            return a.id < b.id;               // stable tiebreak for equal names
        }
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.id < b.id;
    });
    bool changed = false;
    for (size_t i = 0; i < lib.size(); ++i) if (lib[i].id != before[i].id) { changed = true; break; }
    if (!changed) return;
    commit(byName ? L"Sort clips by name" : L"Sort clips by time");
}

void Document::replaceClipBuffer(int id, AudioBufferPtr newBuf, const std::wstring& desc) {
    Clip* c = project_.findClip(id);
    if (!c) return;
    c->buffer = newBuf;
    c->peaks = buildPeaks(newBuf);
    // update placement lengths referencing this clip
    int64_t len = newBuf ? newBuf->frames() : 0;
    for (auto& t : project_.tracks)
        for (auto& p : t.clips)
            if (p.clipId == id) p.lengthFrames = len;
    commit(desc);
}

void Document::replaceClipBuffers(const std::vector<std::pair<int, AudioBufferPtr>>& updates,
                                  const std::wstring& desc) {
    bool any = false;
    for (auto& u : updates) {
        Clip* c = project_.findClip(u.first);
        if (!c) continue;
        c->buffer = u.second;
        c->peaks = buildPeaks(u.second);
        int64_t len = u.second ? u.second->frames() : 0;
        for (auto& t : project_.tracks)
            for (auto& p : t.clips)
                if (p.clipId == u.first) p.lengthFrames = len;
        any = true;
    }
    if (any) commit(desc);
}

int Document::addTrack() {
    Track t;
    t.id = project_.nextTrackId++;
    t.name = L"Track " + std::to_wstring(project_.tracks.size() + 1);
    int id = t.id;
    project_.tracks.push_back(std::move(t));
    commit(L"Add track");
    return id;
}

void Document::removeTrack(int id) {
    auto& v = project_.tracks;
    v.erase(std::remove_if(v.begin(), v.end(),
        [&](const Track& t) { return t.id == id; }), v.end());
    commit(L"Remove track");
}

void Document::renameTrack(int id, const std::wstring& name) {
    Track* t = project_.findTrack(id);
    if (!t || t->name == name) return;
    t->name = name;
    commit(L"Rename track");
}

bool Document::placeClip(int trackId, int clipId, int64_t startFrame, const std::wstring& desc) {
    Track* t = project_.findTrack(trackId);
    Clip* c = project_.findClip(clipId);
    if (!t || !c) return false;
    int64_t len = c->frames();
    if (startFrame < 0) startFrame = 0;
    if (t->overlaps(startFrame, len)) return false;
    PlacedClip p; p.clipId = clipId; p.startFrame = startFrame; p.lengthFrames = len;
    t->clips.push_back(p);
    t->sortClips();
    commit(desc);
    return true;
}

bool Document::moveClip(int fromTrackId, int placedIndex, int toTrackId, int64_t newStart,
                        const std::wstring& desc) {
    Track* from = project_.findTrack(fromTrackId);
    Track* to = project_.findTrack(toTrackId);
    if (!from || !to) return false;
    if (placedIndex < 0 || placedIndex >= (int)from->clips.size()) return false;
    PlacedClip moving = from->clips[placedIndex];
    if (newStart < 0) newStart = 0;
    int ignore = (from == to) ? placedIndex : -1;
    if (to->overlaps(newStart, moving.lengthFrames, ignore)) return false;
    moving.startFrame = newStart;
    from->clips.erase(from->clips.begin() + placedIndex);
    to->clips.push_back(moving);
    to->sortClips();
    from->sortClips();
    commit(desc);
    return true;
}

void Document::removePlaced(int trackId, int placedIndex, const std::wstring& desc) {
    Track* t = project_.findTrack(trackId);
    if (!t || placedIndex < 0 || placedIndex >= (int)t->clips.size()) return;
    t->clips.erase(t->clips.begin() + placedIndex);
    commit(desc);
}

int64_t Document::rippleClips(int trackId, int placedIndex, int64_t delta, const std::wstring& desc) {
    Track* t = project_.findTrack(trackId);
    if (!t) return 0;
    const int64_t applied = t->ripple(placedIndex, delta);
    // A drag that ends up putting everything back where it started, or a gap
    // already at the requested length, shouldn't leave a do-nothing undo step.
    if (applied != 0) commit(desc);
    return applied;
}

void Document::setClipGain(int id, float g, const std::wstring& desc) {
    Clip* c = project_.findClip(id);
    if (!c) return;
    c->gain = std::max(0.0f, std::min(16.0f, g));
    commit(desc);
}

void Document::setTrackGain(int id, float g, const std::wstring& desc) {
    Track* t = project_.findTrack(id);
    if (!t) return;
    t->gain = std::max(0.0f, std::min(16.0f, g));
    commit(desc);
}

int Document::normalizeClips(int onlyClipId, bool acrossAll) {
    // Measure speech loudness of every clip that has audio.
    struct M { int id; double loud; float gain; };
    std::vector<M> ms;
    for (auto& c : project_.library) {
        if (!c.buffer || c.frames() == 0) continue;
        double L = dsp::speechLoudness(*c.buffer);
        if (L < 1e-5) continue;   // effectively silent - skip
        ms.push_back({ c.id, L, c.gain });
    }
    if (ms.empty()) return 0;

    // Target = geometric mean of effective (current) loudness of the reference set.
    // Reference set for "match others" excludes the clip being normalized.
    double sumLogEff = 0; int cnt = 0;
    for (auto& m : ms) {
        if (!acrossAll && onlyClipId >= 0 && m.id == onlyClipId) continue; // exclude self
        sumLogEff += std::log(m.loud * std::max(1e-4f, m.gain));
        ++cnt;
    }
    if (cnt == 0) { // only one clip existed - fall back to unity target
        for (auto& m : ms) { sumLogEff += std::log(m.loud * std::max(1e-4f, m.gain)); ++cnt; }
    }
    double target = std::exp(sumLogEff / cnt);

    int changed = 0;
    for (auto& c : project_.library) {
        if (onlyClipId >= 0 && !acrossAll && c.id != onlyClipId) continue;
        double L = 0;
        for (auto& m : ms) if (m.id == c.id) L = m.loud;
        if (L < 1e-5) continue;
        float g = (float)(target / L);
        g = std::max(0.05f, std::min(16.0f, g));
        if (std::fabs(g - c.gain) > 1e-4f) { c.gain = g; ++changed; }
    }
    if (changed) commit(acrossAll ? L"Normalize all clips" : L"Normalize clip volume");
    return changed;
}

AudioBufferPtr Document::renderMix() const {
    int64_t total = project_.timelineLengthFrames();
    auto out = std::make_shared<AudioBuffer>();
    out->sampleRate = project_.sampleRate;
    out->channels = 2;
    if (total <= 0) return out;
    out->samples.assign((size_t)total * 2, 0.0f);
    for (const auto& t : project_.tracks) {
        if (t.muted) continue;
        for (const auto& pc : t.clips) {
            const Clip* c = project_.findClip(pc.clipId);
            if (!c || !c->buffer) continue;
            float g = c->gain * t.gain;
            int ch = c->buffer->channels;
            const float* s = c->buffer->samples.data();
            int64_t len = std::min<int64_t>(pc.lengthFrames, c->frames());
            for (int64_t i = 0; i < len; ++i) {
                int64_t d = pc.startFrame + i;
                if (d < 0 || d >= total) continue;
                float l = s[i * ch] * g;
                float r = (ch > 1 ? s[i * ch + 1] : s[i * ch]) * g;
                out->samples[d * 2]     += l;
                out->samples[d * 2 + 1] += r;
            }
        }
    }
    for (auto& v : out->samples) v = std::max(-1.0f, std::min(1.0f, v));
    return out;
}

// ---------------- project file I/O (.acep) ----------------
namespace {
    void wU32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
    void wI32(FILE* f, int32_t v) { fwrite(&v, 4, 1, f); }
    void wI64(FILE* f, int64_t v) { fwrite(&v, 8, 1, f); }
    void wF32(FILE* f, float v) { fwrite(&v, 4, 1, f); }
    void wStr(FILE* f, const std::wstring& s) {
        wI32(f, (int32_t)s.size());
        if (!s.empty()) fwrite(s.data(), sizeof(wchar_t), s.size(), f);
    }
    uint32_t rU32(FILE* f) { uint32_t v = 0; fread(&v, 4, 1, f); return v; }
    int32_t rI32(FILE* f) { int32_t v = 0; fread(&v, 4, 1, f); return v; }
    int64_t rI64(FILE* f) { int64_t v = 0; fread(&v, 8, 1, f); return v; }
    float rF32(FILE* f) { float v = 0; fread(&v, 4, 1, f); return v; }
    std::wstring rStr(FILE* f) {
        int32_t n = rI32(f); if (n < 0 || n > 1 << 20) return L"";
        std::wstring s; s.resize(n);
        if (n) fread(&s[0], sizeof(wchar_t), n, f);
        return s;
    }

    // ---- v4: the edit history trailer ----
    //
    // Every undo step holds a whole Project, and a Project's clips point at audio.
    // In memory that costs almost nothing because the snapshots share their
    // AudioBuffers through shared_ptr -- a step is a few hundred bytes of ids and
    // frame counts. Writing them naively would throw that away and put the samples
    // on disk once per step, so a twenty-step history on a 200 MB project would be
    // a multi-gigabyte file.
    //
    // So the file gets a *buffer pool* and snapshots reference it by index. Pool
    // slots 0..library-1 are the current library's buffers, which the v3 body has
    // already written inline; only buffers no longer present in the live project
    // (superseded versions, deleted clips) are appended. Sharing is therefore
    // preserved across the round trip, and loading is cheaper than it would be
    // otherwise because each buffer is read once and handed to every snapshot that
    // wants it.
    //
    // The budget below caps that appended audio. Destructive effects -- timbre
    // matching, noise reduction, normalise -- rewrite every clip they touch, so a
    // handful of them genuinely does multiply the project's size, and a save that
    // silently turned 200 MB into 3 GB would be a worse surprise than a truncated
    // history. Steps that don't fit are still written, by name, without their
    // state: the History window can still say the timbre match happened, it just
    // can't take you back to before it.
    int64_t g_historyAudioBudget = 512ll * 1024 * 1024;

    int64_t bufferBytes(const AudioBufferPtr& b) {
        return b ? (int64_t)b->samples.size() * (int64_t)sizeof(float) : 0;
    }

    // Identity, not equality: two buffers with the same samples are only shared if
    // they are literally the same object, which is exactly the sharing the undo
    // tree creates. Comparing contents would be a hash of every sample on every save.
    using BufferPool = std::vector<AudioBufferPtr>;
    using BufferIndex = std::unordered_map<const AudioBuffer*, int>;

    int poolRef(const BufferIndex& ix, const AudioBufferPtr& b) {
        if (!b) return -1;
        auto it = ix.find(b.get());
        return it == ix.end() ? -1 : it->second;
    }

    void wSnapshot(FILE* f, const Project& p, const BufferIndex& ix) {
        wI32(f, p.sampleRate);
        wI32(f, p.nextClipId);
        wI32(f, p.nextTrackId);
        wI32(f, (int32_t)p.library.size());
        for (const auto& c : p.library) {
            wI32(f, c.id);
            wF32(f, c.gain);
            wStr(f, c.name);
            wStr(f, c.sourcePath);
            wI64(f, (int64_t)c.timestamp);
            wI32(f, poolRef(ix, c.buffer));      // the audio itself lives in the pool
        }
        wI32(f, (int32_t)p.tracks.size());
        for (const auto& t : p.tracks) {
            wI32(f, t.id);
            wF32(f, t.gain);
            wI32(f, t.muted ? 1 : 0);
            wStr(f, t.name);
            wI32(f, (int32_t)t.clips.size());
            for (const auto& pc : t.clips) { wI32(f, pc.clipId); wI64(f, pc.startFrame); wI64(f, pc.lengthFrames); }
        }
    }

    // Peaks are rebuilt per *pool slot*, not per clip, so a buffer shared by twenty
    // snapshots is analysed once and the resulting cache shared exactly as it was
    // before the save.
    bool rSnapshot(FILE* f, Project& p, const BufferPool& pool, const std::vector<PeakCachePtr>& peaks) {
        p = Project{};
        p.sampleRate = rI32(f);
        p.nextClipId = rI32(f);
        p.nextTrackId = rI32(f);
        const int32_t nClips = rI32(f);
        if (nClips < 0 || nClips > 1 << 20) return false;
        for (int i = 0; i < nClips; ++i) {
            Clip c;
            c.id = rI32(f);
            c.gain = rF32(f);
            c.name = rStr(f);
            c.sourcePath = rStr(f);
            c.timestamp = (uint64_t)rI64(f);
            const int32_t ref = rI32(f);
            if (ref >= (int32_t)pool.size()) return false;
            if (ref >= 0) { c.buffer = pool[(size_t)ref]; c.peaks = peaks[(size_t)ref]; }
            else c.peaks = std::make_shared<PeakCache>();
            p.library.push_back(std::move(c));
        }
        const int32_t nTracks = rI32(f);
        if (nTracks < 0 || nTracks > 1 << 20) return false;
        for (int i = 0; i < nTracks; ++i) {
            Track t;
            t.id = rI32(f);
            t.gain = rF32(f);
            t.muted = rI32(f) != 0;
            t.name = rStr(f);
            const int32_t nPl = rI32(f);
            if (nPl < 0 || nPl > 1 << 20) return false;
            for (int j = 0; j < nPl; ++j) {
                PlacedClip pc; pc.clipId = rI32(f); pc.startFrame = rI64(f); pc.lengthFrames = rI64(f);
                t.clips.push_back(pc);
            }
            p.tracks.push_back(std::move(t));
        }
        return true;
    }
}

int64_t Document::historyBudgetBytes() { return g_historyAudioBudget; }
void Document::setHistoryBudgetBytes(int64_t b) { g_historyAudioBudget = b > 0 ? b : 0; }

bool Document::saveProject(const std::wstring& path, bool withHistory) {
    lastHistBytes_ = 0; lastHistDropped_ = 0;

    // Decide what the history costs before writing a byte of it, because the answer
    // determines the version number in the header.
    //
    // Pool slots 0..library-1 are the live library's buffers, written inline by the
    // v3 body below; a snapshot referring to one of those is free. Everything else
    // is superseded audio that has to be appended, so nodes are admitted nearest-
    // first (breadth-first from where we are now) and a node whose buffers would
    // break the budget is written name-only. Nearest-first matters: recent steps
    // are both the most likely to be wanted back and the most likely to share their
    // audio with the current project, so they are the cheapest as well as the best.
    BufferIndex poolIx;
    BufferPool extras;
    std::vector<const UndoNode*> nodes;
    std::vector<char> nodeHasSnapshot;
    int currentIx = -1;

    // Slot numbering has to be something the reader can reproduce without being
    // told, so it is simply the library position -- not a dedup counter, since two
    // library clips sharing one buffer are written (and so read back) as two.
    for (size_t i = 0; i < project_.library.size(); ++i)
        if (project_.library[i].buffer) poolIx.emplace(project_.library[i].buffer.get(), (int)i);
    const int liveSlots = (int)project_.library.size();

    if (withHistory) {
        nodes = undo_.allNodes();
        currentIx = UndoTree::indexOf(nodes, undo_.current());
        if (currentIx < 0) nodes.clear();        // can't say where we are: write none
    }
    if (!nodes.empty()) {
        nodeHasSnapshot.assign(nodes.size(), 0);
        // Breadth-first from the current node over parent *and* child links, so
        // "distance in edits from here" is the admission order in both directions.
        std::vector<int> order;
        {
            std::unordered_map<const UndoNode*, int> at;
            for (int i = 0; i < (int)nodes.size(); ++i) at.emplace(nodes[(size_t)i], i);
            std::vector<char> seen(nodes.size(), 0);
            order.push_back(currentIx); seen[(size_t)currentIx] = 1;
            for (size_t k = 0; k < order.size(); ++k) {
                const UndoNode* n = nodes[(size_t)order[k]];
                auto visit = [&](const UndoNode* m) {
                    if (!m) return;
                    auto it = at.find(m);
                    if (it == at.end() || seen[(size_t)it->second]) return;
                    seen[(size_t)it->second] = 1;
                    order.push_back(it->second);
                };
                visit(n->parent);
                for (const auto& c : n->children) visit(c.get());
            }
        }
        int64_t spent = 0;
        for (int idx : order) {
            const Project& snap = nodes[(size_t)idx]->snapshot;
            if (!nodes[(size_t)idx]->hasSnapshot) continue;   // was name-only already
            // What this step would add on top of everything admitted so far.
            std::vector<AudioBufferPtr> add;
            int64_t cost = 0;
            std::unordered_set<const AudioBuffer*> seenHere;
            for (const auto& c : snap.library) {
                if (!c.buffer || poolIx.count(c.buffer.get())) continue;
                if (!seenHere.insert(c.buffer.get()).second) continue;
                add.push_back(c.buffer);
                cost += bufferBytes(c.buffer);
            }
            if (spent + cost > g_historyAudioBudget && idx != currentIx) continue;
            for (const auto& b : add) { poolIx.emplace(b.get(), liveSlots + (int)extras.size()); extras.push_back(b); }
            spent += cost;
            nodeHasSnapshot[(size_t)idx] = 1;
        }
        lastHistBytes_ = spent;
        for (size_t i = 0; i < nodes.size(); ++i) if (!nodeHasSnapshot[i]) ++lastHistDropped_;
    }

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite("ACEP", 1, 4, f);
    // v4 only when there is a history to read; a plain save stays byte-for-byte the
    // file older builds wrote, so "save without history" is also "stay compatible".
    wU32(f, nodes.empty() ? 3u : 4u);   // v2: per-clip timestamp (sort by time); v3: trailing view state
    wI32(f, project_.sampleRate);
    wI32(f, project_.nextClipId);
    wI32(f, project_.nextTrackId);
    wI32(f, (int32_t)project_.library.size());
    for (auto& c : project_.library) {
        wI32(f, c.id);
        wF32(f, c.gain);
        wStr(f, c.name);
        wStr(f, c.sourcePath);
        wI64(f, (int64_t)c.timestamp);
        int ch = c.buffer ? c.buffer->channels : 2;
        int sr = c.buffer ? c.buffer->sampleRate : project_.sampleRate;
        int64_t nf = c.frames();
        wI32(f, ch); wI32(f, sr); wI64(f, nf);
        if (c.buffer && !c.buffer->samples.empty())
            fwrite(c.buffer->samples.data(), sizeof(float), c.buffer->samples.size(), f);
    }
    wI32(f, (int32_t)project_.tracks.size());
    for (auto& t : project_.tracks) {
        wI32(f, t.id);
        wF32(f, t.gain);
        wI32(f, t.muted ? 1 : 0);
        wStr(f, t.name);
        wI32(f, (int32_t)t.clips.size());
        for (auto& pc : t.clips) { wI32(f, pc.clipId); wI64(f, pc.startFrame); wI64(f, pc.lengthFrames); }
    }
    // v3: view state (selection + playhead) last, so older readers that stop
    // after the tracks still load everything they understand.
    wI32(f, view_.selClipId);
    wI64(f, view_.selStart);
    wI64(f, view_.selEnd);
    wI64(f, view_.playheadFrame);

    // v4: the edit history. Appended after everything a v3 reader knows about, so
    // the compatibility story is "an old build opens the project and just doesn't
    // see the history" rather than "an old build can't open it at all".
    if (!nodes.empty()) {
        wI32(f, (int32_t)extras.size());
        for (const auto& b : extras) {
            wI32(f, b->channels); wI32(f, b->sampleRate); wI64(f, (int64_t)b->frames());
            if (!b->samples.empty()) fwrite(b->samples.data(), sizeof(float), b->samples.size(), f);
        }
        std::unordered_map<const UndoNode*, int> at;
        for (int i = 0; i < (int)nodes.size(); ++i) at.emplace(nodes[(size_t)i], i);
        wI32(f, (int32_t)nodes.size());
        wI32(f, currentIx);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const UndoNode* n = nodes[i];
            const auto p = n->parent ? at.find(n->parent) : at.end();
            wI32(f, n->parent && p != at.end() ? p->second : -1);
            wI32(f, n->lastChild);
            wI32(f, nodeHasSnapshot[i] ? 1 : 0);
            wStr(f, n->desc);
            if (nodeHasSnapshot[i]) wSnapshot(f, n->snapshot, poolIx);
        }
    }

    fclose(f);
    markSaved();
    return true;
}

bool Document::loadProject(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[4]; if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ACEP", 4) != 0) { fclose(f); return false; }
    uint32_t ver = rU32(f);

    Project p;
    p.sampleRate = rI32(f);
    p.nextClipId = rI32(f);
    p.nextTrackId = rI32(f);
    int32_t nClips = rI32(f);
    for (int i = 0; i < nClips; ++i) {
        Clip c;
        c.id = rI32(f);
        c.gain = rF32(f);
        c.name = rStr(f);
        c.sourcePath = rStr(f);
        // v2+ stores the timestamp; for older projects fall back to the source
        // file's current mtime (or now) so "sort by time" still does something sane.
        c.timestamp = (ver >= 2) ? (uint64_t)rI64(f) : clipTimestampFor(c.sourcePath);
        int ch = rI32(f); int sr = rI32(f); int64_t nf = rI64(f);
        auto buf = std::make_shared<AudioBuffer>();
        buf->channels = ch; buf->sampleRate = sr;
        buf->samples.resize((size_t)nf * ch);
        if (nf > 0) fread(buf->samples.data(), sizeof(float), buf->samples.size(), f);
        c.buffer = buf;
        c.peaks = buildPeaks(buf);
        p.library.push_back(std::move(c));
    }
    int32_t nTracks = rI32(f);
    for (int i = 0; i < nTracks; ++i) {
        Track t;
        t.id = rI32(f);
        t.gain = rF32(f);
        t.muted = rI32(f) != 0;
        t.name = rStr(f);
        int32_t nPl = rI32(f);
        for (int j = 0; j < nPl; ++j) {
            PlacedClip pc; pc.clipId = rI32(f); pc.startFrame = rI64(f); pc.lengthFrames = rI64(f);
            t.clips.push_back(pc);
        }
        p.tracks.push_back(std::move(t));
    }
    ViewState v;
    if (ver >= 3) {
        v.selClipId = rI32(f);
        v.selStart = rI64(f);
        v.selEnd = rI64(f);
        v.playheadFrame = rI64(f);
    }

    // v4: the edit history. Read into locals and only adopted if it comes back
    // whole -- a history that fails to parse must cost the user the history, never
    // the project, which is sitting complete in `p` by this point.
    std::vector<UndoTree::FlatNode> flat;
    int histCurrent = -1;
    if (ver >= 4) {
        // Pool slots 0..library-1 mirror the clips just read, so a snapshot that
        // refers to audio the project still uses shares it rather than copying.
        BufferPool pool;
        std::vector<PeakCachePtr> peaks;
        for (const auto& c : p.library) { pool.push_back(c.buffer); peaks.push_back(c.peaks); }
        const int32_t nExtra = rI32(f);
        bool ok = nExtra >= 0 && nExtra <= 1 << 20;
        for (int i = 0; ok && i < nExtra; ++i) {
            int ch = rI32(f); int sr = rI32(f); int64_t nf = rI64(f);
            if (ch <= 0 || ch > 8 || nf < 0 || nf > (int64_t)1 << 34) { ok = false; break; }
            auto buf = std::make_shared<AudioBuffer>();
            buf->channels = ch; buf->sampleRate = sr;
            buf->samples.resize((size_t)nf * ch);
            if (nf > 0 && fread(buf->samples.data(), sizeof(float), buf->samples.size(), f)
                          != buf->samples.size()) { ok = false; break; }
            pool.push_back(buf);
            peaks.push_back(buildPeaks(buf));
        }
        if (ok) {
            const int32_t nNodes = rI32(f);
            histCurrent = rI32(f);
            ok = nNodes > 0 && nNodes <= 1 << 20;
            for (int i = 0; ok && i < nNodes; ++i) {
                UndoTree::FlatNode fn;
                fn.parent = rI32(f);
                fn.lastChild = rI32(f);
                fn.hasSnapshot = rI32(f) != 0;
                fn.desc = rStr(f);
                if (fn.hasSnapshot && !rSnapshot(f, fn.snapshot, pool, peaks)) { ok = false; break; }
                flat.push_back(std::move(fn));
            }
        }
        if (!ok) { flat.clear(); histCurrent = -1; }
    }
    fclose(f);
    if (p.tracks.empty()) { Track t; t.id = p.nextTrackId++; t.name = L"Track 1"; p.tracks.push_back(t); }

    clampSelection(p, v.selClipId, v.selStart, v.selEnd);
    v.playheadFrame = std::max<int64_t>(0, v.playheadFrame);

    view_ = v;
    project_ = p;
    if (flat.empty() || !undo_.rebuild(flat, histCurrent))
        undo_.init(project_);   // no history in the file, or it didn't survive the trip
    else
        // The file's own copy of the project is authoritative over the snapshot at
        // the current node: they are written from the same state, but only one of
        // them is what every other reader of this file will see.
        undo_.current()->snapshot = project_;
    markSaved();
    return true;
}

std::wstring Document::undoDesc() const {
    return undo_.current() ? undo_.current()->desc : L"";
}

void Document::undo() {
    if (!undo_.canUndo()) return;
    project_ = undo_.undo();
}

void Document::redo(int branch) {
    if (!undo_.canRedo()) return;
    if (branch < 0 || branch >= undo_.redoBranchCount()) branch = undo_.defaultRedoBranch();
    project_ = undo_.redo(branch);
}

std::vector<Document::HistoryEntry> Document::history() const {
    int cur = 0;
    const std::vector<const UndoNode*> chain = undo_.chain(&cur);
    std::vector<HistoryEntry> out;
    out.reserve(chain.size());
    for (int i = 0; i < (int)chain.size(); ++i) {
        HistoryEntry e;
        e.desc = chain[(size_t)i]->desc;
        e.applied = i <= cur;
        e.current = i == cur;
        e.saved = chain[(size_t)i] == savedNode_;
        e.branches = (int)chain[(size_t)i]->children.size();
        e.restorable = chain[(size_t)i]->hasSnapshot;
        out.push_back(std::move(e));
    }
    return out;
}

int Document::historyIndex() const {
    int cur = 0;
    undo_.chain(&cur);
    return cur;
}

// The index is re-resolved against a freshly built chain rather than taking a node
// pointer from the caller: the History window hands back a row number it was given
// earlier, and a node pointer held across that boundary would be a dangling one the
// moment anything committed in between.
bool Document::gotoHistory(int index) {
    int cur = 0;
    const std::vector<const UndoNode*> chain = undo_.chain(&cur);
    if (index < 0 || index >= (int)chain.size() || index == cur) return false;
    if (!chain[(size_t)index]->hasSnapshot) return false;   // listed, but no state to go to
    project_ = undo_.gotoNode(chain[(size_t)index]);
    return true;
}
