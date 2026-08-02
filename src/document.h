#pragma once
#include "model.h"
#include "undo.h"
#include <string>
#include <vector>
#include <utility>

// Where the user was working: the active waveform selection and the playhead.
// This is UI state, not project data — it rides along in the .acep file (so
// reopening a project puts you back where you left off) and is deliberately
// *outside* the undo tree (dragging a selection must not create an undo step,
// and undo/redo must not yank the selection around). It does count towards the
// unsaved-changes flag, though: a selection you took the trouble to make is work
// you'd hate to lose, so changing it earns a `*` and a save prompt on exit. The
// playhead is the exception — it's saved but excluded from the dirty check,
// since it advances by itself during playback.
struct ViewState {
    int     selClipId = -1;      // clip that owns the selection, -1 = none
    int64_t selStart = 0;        // selection bounds, in frames of that clip
    int64_t selEnd = 0;
    int64_t playheadFrame = 0;   // timeline playhead
};

// Trim a selection to what `project` can actually honour, and drop it (clipId = -1,
// bounds zeroed) when nothing of it survives — its clip is gone, or the range no
// longer overlaps the clip's audio.
//
// A selection is work the user did by hand, so it is only ever narrowed by this,
// never discarded as a blanket precaution. Both places that can invalidate one go
// through here — loading a project, and any edit that may have replaced or removed
// clips — so the two rules cannot drift apart.
void clampSelection(const Project& project, int& clipId, int64_t& start, int64_t& end);

// Owns the project state and the undo/redo tree. Every mutation goes through a
// method here that records a snapshot, so Ctrl+Z can undo anything.
class Document {
public:
    void init(int sampleRate);

    Project& project() { return project_; }
    const Project& project() const { return project_; }

    // Saved/loaded with the project; see ViewState. The UI syncs its live
    // selection in before saving and reads it back after loading.
    ViewState& view() { return view_; }
    const ViewState& view() const { return view_; }

    static PeakCachePtr buildPeaks(const AudioBufferPtr& buf);

    // --- Library operations (each commits an undo step) ---
    int  addClip(const std::wstring& name, AudioBufferPtr buf,
                 const std::wstring& sourcePath, const std::wstring& undoDesc);
    void renameClip(int id, const std::wstring& name);
    void removeClip(int id);
    // Reorder the library: move `clipId` so it lands at `targetIndex` in reading
    // order (as if the item hadn't been removed first). One undo step; no-op if the
    // position doesn't actually change.
    void moveClipInLibrary(int clipId, int targetIndex);
    // Sort the library by clip name (A→Z) or by timestamp (oldest→newest). One undo step.
    void sortLibrary(bool byName);
    void replaceClipBuffer(int id, AudioBufferPtr newBuf, const std::wstring& desc);
    // Replace several clip buffers in one undo step (e.g. denoise a whole track).
    void replaceClipBuffers(const std::vector<std::pair<int, AudioBufferPtr>>& updates,
                            const std::wstring& desc);

    // --- Volume ---
    void setClipGain(int id, float g, const std::wstring& desc);
    void setTrackGain(int id, float g, const std::wstring& desc);
    // Speech-aware normalization (ignores silent gaps). matchTarget from either
    // the other clips only, or every clip. Returns number of clips changed.
    int normalizeClips(int onlyClipId /* -1 = all */, bool acrossAll);

    // --- Project I/O and mixdown ---
    // `withHistory` writes the undo tree into the file (format v4) so reopening the
    // project can still answer "did I apply that?". It costs disk: every superseded
    // version of a clip's audio that the history still refers to has to be stored,
    // deduplicated but uncompressed. Passing false writes a v3-shaped file holding
    // only the current state, which is what the older versions of this app wrote.
    bool saveProject(const std::wstring& path, bool withHistory = true);
    bool loadProject(const std::wstring& path);
    // Bytes of *superseded* audio the last save wrote (0 when the history fitted in
    // what the project already contains, which is the common case), and how many
    // steps had to be stored name-only because the budget ran out.
    int64_t lastSaveHistoryBytes() const { return lastHistBytes_; }
    int  lastSaveHistoryDropped() const { return lastHistDropped_; }
    // How much superseded audio a save may store before it starts recording steps
    // by name only. Settable so the trimming path can be exercised without building
    // a half-gigabyte fixture -- it is the fiddliest part of the format, so being
    // able to test it at a budget of a few hundred bytes is worth the knob.
    static int64_t historyBudgetBytes();
    static void setHistoryBudgetBytes(int64_t bytes);
    AudioBufferPtr renderMix() const;   // all tracks mixed to one stereo buffer

    // Commit a snapshot after direct live edits (e.g. dragging a volume slider).
    void commitEdit(const std::wstring& desc) { commit(desc); }

    // --- Track operations ---
    int  addTrack();
    void removeTrack(int id);
    void renameTrack(int id, const std::wstring& name);
    bool placeClip(int trackId, int clipId, int64_t startFrame, const std::wstring& desc);
    bool moveClip(int fromTrackId, int placedIndex, int toTrackId, int64_t newStart,
                  const std::wstring& desc);
    void removePlaced(int trackId, int placedIndex, const std::wstring& desc);
    // Slide a placed clip and everything after it on the same track by `delta`
    // frames, preserving the spacing between them (see Track::ripple). This is
    // how the gap in front of a clip is changed without disturbing the timing of
    // the arrangement downstream. Returns the delta actually applied, which is
    // clamped when sliding left; commits an undo step only if anything moved.
    int64_t rippleClips(int trackId, int placedIndex, int64_t delta, const std::wstring& desc);

    // --- Undo / redo ---
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }
    std::wstring undoDesc() const;
    void undo();

    int  redoBranchCount() const { return undo_.redoBranchCount(); }
    std::wstring redoChildDesc(int i) const { return undo_.redoChildDesc(i); }
    bool redoBranchRestorable(int i) const { return undo_.redoBranchRestorable(i); }
    int  defaultRedoBranch() const { return undo_.defaultRedoBranch(); }
    void redo(int branch);

    // --- History (what the History window lists) ---
    // One row per state on the undo/redo chain, oldest first. Effects like timbre
    // matching change audio without changing anything you can see in the arrangement,
    // so "did I apply that, and is it still applied?" is otherwise unanswerable --
    // this is the answer: every step by name, which of them are in effect, and where
    // the last save falls among them.
    struct HistoryEntry {
        std::wstring desc;      // what the step did ("Match timbre of 23 clips ...")
        bool applied = false;   // in effect right now (at or before the current state)
        bool current = false;   // the state the project is in
        bool saved = false;     // the state the file on disk holds
        int  branches = 0;      // redo children; >1 means alternatives not listed here
        // False for a step read back from a project whose history exceeded the
        // storage budget: still named here (which answers "was this done?"), but
        // its state was not stored, so it can't be gone back to.
        bool restorable = true;
    };
    std::vector<HistoryEntry> history() const;
    // Where in history() the project currently is.
    int historyIndex() const;
    // Move to history()[index]; false if the index is out of range or already current.
    bool gotoHistory(int index);

    // Identifies *where* in the history we currently are. The UI keeps its own
    // selection-undo stack alongside this one and uses the node's identity to tell
    // when the document has moved out from under it (see selection history in
    // ui.cpp); the same pointer-as-a-bookmark trick as savedNode_ below.
    const UndoNode* historyNode() const { return undo_.current(); }

    int newClipId();  // allocate without committing (caller commits)

    // --- Unsaved-changes tracking ---
    // The "saved" marker is the undo node that was current when the project was
    // last saved (or freshly loaded/created), plus the selection as it stood at
    // that moment. The project is modified iff the current undo node differs — so
    // undoing back to the saved state clears the dirty flag and redoing away sets
    // it again — or the selection has moved since. The playhead is intentionally
    // not compared: it advances on its own during playback, so counting it would
    // dirty the project just for pressing Play.
    void markSaved() { savedNode_ = undo_.current(); savedView_ = view_; }
    bool isModified() const {
        return undo_.current() != savedNode_ ||
               view_.selClipId != savedView_.selClipId ||
               view_.selStart  != savedView_.selStart ||
               view_.selEnd    != savedView_.selEnd;
    }

private:
    void commit(const std::wstring& desc) { undo_.commit(project_, desc); }

    Project project_;
    ViewState view_;
    UndoTree undo_;
    const UndoNode* savedNode_ = nullptr;
    ViewState savedView_;            // selection as of the last save/load/new
    int64_t lastHistBytes_ = 0;      // reporting for the last saveProject
    int lastHistDropped_ = 0;
};
