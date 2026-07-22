#pragma once
#include "model.h"
#include "undo.h"
#include <string>
#include <vector>
#include <utility>

// Owns the project state and the undo/redo tree. Every mutation goes through a
// method here that records a snapshot, so Ctrl+Z can undo anything.
class Document {
public:
    void init(int sampleRate);

    Project& project() { return project_; }
    const Project& project() const { return project_; }

    static PeakCachePtr buildPeaks(const AudioBufferPtr& buf);

    // --- Library operations (each commits an undo step) ---
    int  addClip(const std::wstring& name, AudioBufferPtr buf,
                 const std::wstring& sourcePath, const std::wstring& undoDesc);
    void renameClip(int id, const std::wstring& name);
    void removeClip(int id);
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
    bool saveProject(const std::wstring& path);
    bool loadProject(const std::wstring& path);
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

    // --- Undo / redo ---
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }
    std::wstring undoDesc() const;
    void undo();

    int  redoBranchCount() const { return undo_.redoBranchCount(); }
    std::wstring redoChildDesc(int i) const { return undo_.redoChildDesc(i); }
    int  defaultRedoBranch() const { return undo_.defaultRedoBranch(); }
    void redo(int branch);

    int newClipId();  // allocate without committing (caller commits)

    // --- Unsaved-changes tracking ---
    // The "saved" marker is the undo node that was current when the project was
    // last saved (or freshly loaded/created). The project is modified iff the
    // current undo node differs from it — so undoing back to the saved state
    // clears the dirty flag, and redoing away sets it again.
    void markSaved() { savedNode_ = undo_.current(); }
    bool isModified() const { return undo_.current() != savedNode_; }

private:
    void commit(const std::wstring& desc) { undo_.commit(project_, desc); }

    Project project_;
    UndoTree undo_;
    const UndoNode* savedNode_ = nullptr;
};
