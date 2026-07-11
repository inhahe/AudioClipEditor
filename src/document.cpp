#include "document.h"
#include "dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdint>

void Document::init(int sampleRate) {
    project_ = Project{};
    project_.sampleRate = sampleRate;
    // start with a single track
    Track t;
    t.id = project_.nextTrackId++;
    t.name = L"Track 1";
    project_.tracks.push_back(std::move(t));
    undo_.init(project_);
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
}

bool Document::saveProject(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite("ACEP", 1, 4, f);
    wU32(f, 1);
    wI32(f, project_.sampleRate);
    wI32(f, project_.nextClipId);
    wI32(f, project_.nextTrackId);
    wI32(f, (int32_t)project_.library.size());
    for (auto& c : project_.library) {
        wI32(f, c.id);
        wF32(f, c.gain);
        wStr(f, c.name);
        wStr(f, c.sourcePath);
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
    fclose(f);
    return true;
}

bool Document::loadProject(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[4]; if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ACEP", 4) != 0) { fclose(f); return false; }
    uint32_t ver = rU32(f); (void)ver;

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
    fclose(f);
    if (p.tracks.empty()) { Track t; t.id = p.nextTrackId++; t.name = L"Track 1"; p.tracks.push_back(t); }

    project_ = p;
    undo_.init(project_);   // fresh history for loaded project
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
