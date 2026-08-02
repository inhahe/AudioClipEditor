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
    // False only for steps read back from a project file whose history was too
    // large to store in full (see the audio budget in document.cpp). The step is
    // still *listed* -- its name is the whole point of the History window, and a
    // name costs a few bytes -- but its project state was never written, so it
    // cannot be undone to. Everything that can reach a node has to check this.
    bool hasSnapshot = true;

    // Tear the tree down iteratively. Letting unique_ptr do it would recurse once
    // per node, and a history is one long thin chain -- so that is one stack frame
    // per edit the project has ever recorded. That was survivable while a history
    // lasted only as long as the session; now that projects carry theirs across
    // reopens it grows without bound, and the crash would land on whoever had used
    // the app the longest.
    ~UndoNode() {
        std::vector<std::unique_ptr<UndoNode>> pending;
        pending.swap(children);
        while (!pending.empty()) {
            std::unique_ptr<UndoNode> n = std::move(pending.back());
            pending.pop_back();
            for (auto& c : n->children) pending.push_back(std::move(c));
            n->children.clear();   // so ~UndoNode on `n` finds nothing left to do
        }
    }
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

    // A step with no stored snapshot is a dead end in both directions: there is
    // no state to put the project into, so it is listed but not walked to.
    bool canUndo() const { return current_ && current_->parent && current_->parent->hasSnapshot; }
    bool canRedo() const { return defaultRedoBranch() >= 0; }
    bool redoBranchRestorable(int i) const {
        return current_ && i >= 0 && i < (int)current_->children.size() &&
               current_->children[(size_t)i]->hasSnapshot;
    }

    // Move to parent; returns the project state to restore. Caller checks canUndo().
    const Project& undo(std::wstring* undoneDesc = nullptr) {
        if (undoneDesc) *undoneDesc = current_->desc;
        current_ = current_->parent;
        return current_->snapshot;
    }

    int redoBranchCount() const { return current_ ? (int)current_->children.size() : 0; }
    std::wstring redoChildDesc(int i) const { return current_->children[i]->desc; }
    int defaultRedoBranch() const {
        if (!current_ || current_->children.empty()) return -1;
        if (redoBranchRestorable(current_->lastChild)) return current_->lastChild;
        for (int i = 0; i < (int)current_->children.size(); ++i)
            if (redoBranchRestorable(i)) return i;
        return -1;
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

    // ---- serialisation support (see the .acep v4 trailer in document.cpp) ----

    // The whole tree, not just the walkable chain, flattened parent-before-child.
    // That ordering is the point: a reader can rebuild the tree from nothing but a
    // parent index per node, because a node's parent is always already built.
    // Iterative rather than recursive: a history is usually one long thin chain,
    // so recursion here would be one stack frame per edit ever made.
    std::vector<const UndoNode*> allNodes() const {
        std::vector<const UndoNode*> out;
        if (!root_) return out;
        out.push_back(root_.get());
        for (size_t i = 0; i < out.size(); ++i)             // grows as it walks
            for (const auto& c : out[i]->children) out.push_back(c.get());
        return out;
    }
    // Position of `n` in allNodes() order, or -1. Used to write the "where are we"
    // marker, and small enough that a linear scan is not worth avoiding.
    static int indexOf(const std::vector<const UndoNode*>& nodes, const UndoNode* n) {
        for (int i = 0; i < (int)nodes.size(); ++i) if (nodes[(size_t)i] == n) return i;
        return -1;
    }

    struct FlatNode {
        int parent = -1;            // index into the flat vector; -1 = root
        int lastChild = -1;
        bool hasSnapshot = true;
        std::wstring desc;
        Project snapshot;           // ignored when hasSnapshot is false
    };
    // Rebuild a tree from the flat form. Rejects anything malformed rather than
    // building a half-tree: a corrupt history is not worth risking the project for,
    // and the caller falls back to a fresh one-step history.
    bool rebuild(const std::vector<FlatNode>& flat, int currentIndex) {
        if (flat.empty() || flat[0].parent != -1) return false;
        if (currentIndex < 0 || currentIndex >= (int)flat.size()) return false;
        if (!flat[(size_t)currentIndex].hasSnapshot) return false;
        for (size_t i = 1; i < flat.size(); ++i)            // parent-before-child
            if (flat[i].parent < 0 || flat[i].parent >= (int)i) return false;
        for (size_t i = 0; i < flat.size(); ++i) {
            const int lc = flat[i].lastChild;
            if (lc < -1) return false;
        }

        std::vector<UndoNode*> built(flat.size(), nullptr);
        auto newRoot = std::make_unique<UndoNode>();
        newRoot->desc = flat[0].desc;
        newRoot->lastChild = flat[0].lastChild;
        newRoot->hasSnapshot = flat[0].hasSnapshot;
        if (flat[0].hasSnapshot) newRoot->snapshot = flat[0].snapshot;
        built[0] = newRoot.get();
        for (size_t i = 1; i < flat.size(); ++i) {
            auto node = std::make_unique<UndoNode>();
            node->desc = flat[i].desc;
            node->lastChild = flat[i].lastChild;
            node->hasSnapshot = flat[i].hasSnapshot;
            if (flat[i].hasSnapshot) node->snapshot = flat[i].snapshot;
            node->parent = built[(size_t)flat[i].parent];
            built[i] = node.get();
            node->parent->children.push_back(std::move(node));
        }
        // lastChild was written against the child *order*, which push_back above
        // reproduces exactly; anything out of range is clamped away rather than
        // trusted, since it only ever selects a default redo branch.
        for (size_t i = 0; i < flat.size(); ++i)
            if (built[i]->lastChild >= (int)built[i]->children.size()) built[i]->lastChild = -1;

        root_ = std::move(newRoot);
        current_ = built[(size_t)currentIndex];
        return true;
    }

private:

    std::unique_ptr<UndoNode> root_;
    UndoNode* current_ = nullptr;
};
