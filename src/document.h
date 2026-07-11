#pragma once
#include "model.h"
#include "undo.h"
#include <string>

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

private:
    void commit(const std::wstring& desc) { undo_.commit(project_, desc); }

    Project project_;
    UndoTree undo_;
};
