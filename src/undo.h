#pragma once
#include "model.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <string>

// Undo history as a tree. Each node holds a full Project snapshot (cheap: audio
// buffers are shared via shared_ptr, only the small vectors are duplicated).
// Redo can branch: if a node has multiple children, the UI offers a picker.
struct UndoNode {
    std::wstring desc;              // what this state represents ("Crop 'foo'", ...)
    Project snapshot;              // project state AFTER this edit
    UndoNode* parent = nullptr;
    std::vector<std::unique_ptr<UndoNode>> children;
    int lastChild = -1;            // most-recently-followed redo branch
};

class UndoTree {
public:
    // Initialize with the starting project state (root = "New project").
    void init(const Project& initial) {
        root_ = std::make_unique<UndoNode>();
        root_->desc = L"New project";
        root_->snapshot = initial;
        current_ = root_.get();
    }

    // Record a new state as a child of the current node and move there.
    void commit(const Project& newState, const std::wstring& desc) {
        auto node = std::make_unique<UndoNode>();
        node->desc = desc;
        node->snapshot = newState;
        node->parent = current_;
        current_->children.push_back(std::move(node));
        current_->lastChild = (int)current_->children.size() - 1;
        current_ = current_->children.back().get();
    }

    bool canUndo() const { return current_ && current_->parent; }
    bool canRedo() const { return current_ && !current_->children.empty(); }

    // Move to parent; returns the project state to restore. Caller checks canUndo().
    const Project& undo(std::wstring* undoneDesc = nullptr) {
        if (undoneDesc) *undoneDesc = current_->desc;
        current_ = current_->parent;
        return current_->snapshot;
    }

    int redoBranchCount() const { return current_ ? (int)current_->children.size() : 0; }
    std::wstring redoChildDesc(int i) const { return current_->children[i]->desc; }
    int defaultRedoBranch() const {
        if (!canRedo()) return -1;
        return current_->lastChild >= 0 ? current_->lastChild : 0;
    }

    // Follow a specific redo branch; returns the project state to restore.
    const Project& redo(int branch) {
        current_->lastChild = branch;
        current_ = current_->children[branch].get();
        return current_->snapshot;
    }

    UndoNode* current() const { return current_; }
    UndoNode* rootNode() const { return root_.get(); }

    // The single line of history the user can see and walk: root -> ... -> current
    // (everything that is applied right now), then on past current down the
    // remembered redo trail to the tip (everything that was undone but can still be
    // redone). That is exactly the set of states reachable by pressing Ctrl+Z and
    // Ctrl+Shift+Z alone, which is what makes it the right thing to show as a list.
    // Branches hanging off the side are not included -- they're reachable only via
    // the redo picker -- but `children.size() > 1` on a listed node tells the UI to
    // say so. `currentIndex` receives the position of the current node.
    std::vector<const UndoNode*> chain(int* currentIndex = nullptr) const {
        std::vector<const UndoNode*> out;
        for (const UndoNode* n = current_; n; n = n->parent) out.push_back(n);
        std::reverse(out.begin(), out.end());
        if (currentIndex) *currentIndex = (int)out.size() - 1;
        for (const UndoNode* n = current_; n && !n->children.empty(); ) {
            const int b = n->lastChild >= 0 ? n->lastChild : 0;
            n = n->children[(size_t)b].get();
            out.push_back(n);
        }
        return out;
    }

    // Jump straight to a node (the History window's click-to-go). Equivalent to
    // pressing undo/redo the right number of times, so it also re-points the redo
    // trail at the target: after jumping back, undoing further and then redoing
    // must return the way it came, not down whichever branch happened to be last.
    const Project& gotoNode(const UndoNode* target) {
        UndoNode* t = const_cast<UndoNode*>(target);
        for (UndoNode* n = t; n && n->parent; n = n->parent) {
            UndoNode* p = n->parent;
            for (int i = 0; i < (int)p->children.size(); ++i)
                if (p->children[(size_t)i].get() == n) { p->lastChild = i; break; }
        }
        current_ = t;
        return current_->snapshot;
    }

private:
    std::unique_ptr<UndoNode> root_;
    UndoNode* current_ = nullptr;
};
