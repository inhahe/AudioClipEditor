#include "document.h"

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
