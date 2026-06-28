// pairingheap.hpp — Pairing heap.
//
// Mirrors PostgreSQL's src/backend/lib/pairingheap.c. A standard pairing
// heap (Fredman et al. 1986) with the two-pass delete-min variant. The
// representation is leftmost-child / next-sibling with a `prev` pointer that
// points to either the previous sibling or, for the leftmost child, the
// parent. Compare(a, b) returns true if a should appear before b at the top
// of the heap; pass std::less<T> for a min-heap, std::greater<T> for a max.
//
// Operations (amortized complexity):
//   Add: O(1)
//   Top: O(1)
//   Pop: O(log n) amortized (two-pass pairing)
//   Meld: O(1)  (merges another heap into this one)
//
// The heap owns all internal nodes; values are returned by move on Pop.

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace mytoydb::lib {

template<typename T, typename Compare = std::less<T>>
class PairingHeap {
public:
    PairingHeap() = default;
    explicit PairingHeap(Compare cmp) : cmp_(std::move(cmp)) {}

    // Destroys all owned nodes.
    ~PairingHeap() { Clear(); }

    PairingHeap(const PairingHeap&) = delete;
    PairingHeap& operator=(const PairingHeap&) = delete;

    PairingHeap(PairingHeap&& other) noexcept
        : root_(other.root_), size_(other.size_), cmp_(std::move(other.cmp_)) {
        other.root_ = nullptr;
        other.size_ = 0;
    }
    PairingHeap& operator=(PairingHeap&& other) noexcept {
        if (this != &other) {
            Clear();
            root_ = other.root_;
            size_ = other.size_;
            cmp_ = std::move(other.cmp_);
            other.root_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // PG: pairingheap_add.
    void Add(const T& value);
    void Add(T&& value);

    // PG: pairingheap_first — read the topmost element without removing it.
    const T& Top() const;

    // PG: pairingheap_remove_first — remove the topmost element. Returns
    // true and writes the value to *out on success; returns false if empty.
    bool Pop(T* out);

    std::size_t Size() const { return size_; }
    bool IsEmpty() const { return size_ == 0; }

    // Drop all nodes and release their memory.
    void Clear();

    // PG: pairingheap_combine — merge another heap into this one. The other
    // heap becomes empty after the call. Both heaps must share the same
    // comparator instance for the merge to be well-defined.
    void Meld(PairingHeap& other);

private:
    struct Node {
        T value;
        Node* child;    // leftmost child
        Node* sibling;  // next sibling (right sibling)
        // For the leftmost child of a parent, prev points to the parent.
        // For other children, prev points to the previous sibling.
        Node* prev;

        template<typename U>
        explicit Node(U&& v)
            : value(std::forward<U>(v)), child(nullptr), sibling(nullptr), prev(nullptr) {}
    };

    Node* root_ = nullptr;
    std::size_t size_ = 0;
    Compare cmp_{};

    // Meld two trees (single roots). Returns the new root. Either argument
    // may be nullptr; the function takes ownership of both.
    static Node* MeldTrees(Node* a, Node* b, const Compare& cmp);

    // Two-pass pairing of the children list of root. Returns the new root
    // (which may be nullptr if first was nullptr).
    static Node* TwoPassMeld(Node* first, const Compare& cmp);

    void DestroySubtree(Node* node);
};

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------

template<typename T, typename Compare>
typename PairingHeap<T, Compare>::Node* PairingHeap<T, Compare>::MeldTrees(Node* a, Node* b,
                                                                           const Compare& cmp) {
    if (a == nullptr)
        return b;
    if (b == nullptr)
        return a;
    // Ensure a holds the smaller value (i.e., a should be the new root).
    if (cmp(b->value, a->value)) {
        std::swap(a, b);
    }
    // Make b the new leftmost child of a.
    b->sibling = a->child;
    if (a->child != nullptr) {
        a->child->prev = b;
    }
    b->prev = a;  // leftmost child's prev points to parent
    a->child = b;
    a->sibling = nullptr;
    a->prev = nullptr;
    return a;
}

template<typename T, typename Compare>
typename PairingHeap<T, Compare>::Node* PairingHeap<T, Compare>::TwoPassMeld(Node* first,
                                                                             const Compare& cmp) {
    if (first == nullptr) {
        return nullptr;
    }

    // Collect the children into a list (in left-to-right order).
    std::vector<Node*> trees;
    for (Node* cur = first; cur != nullptr;) {
        Node* next = cur->sibling;
        cur->prev = nullptr;
        cur->sibling = nullptr;
        trees.push_back(cur);
        cur = next;
    }

    // Pass 1: pair up adjacent trees and meld them in pairs.
    std::vector<Node*> merged;
    merged.reserve(trees.size() / 2 + 1);
    for (std::size_t i = 0; i < trees.size(); i += 2) {
        if (i + 1 < trees.size()) {
            merged.push_back(MeldTrees(trees[i], trees[i + 1], cmp));
        } else {
            merged.push_back(trees[i]);
        }
    }

    // Pass 2: meld the resulting trees from right to left.
    Node* result = merged.back();
    for (std::size_t i = merged.size() - 1; i-- > 0;) {
        result = MeldTrees(merged[i], result, cmp);
    }
    return result;
}

template<typename T, typename Compare>
void PairingHeap<T, Compare>::Add(const T& value) {
    Node* node = new Node(value);
    root_ = MeldTrees(root_, node, cmp_);
    ++size_;
}

template<typename T, typename Compare>
void PairingHeap<T, Compare>::Add(T&& value) {
    Node* node = new Node(std::move(value));
    root_ = MeldTrees(root_, node, cmp_);
    ++size_;
}

template<typename T, typename Compare>
const T& PairingHeap<T, Compare>::Top() const {
    return root_->value;
}

template<typename T, typename Compare>
bool PairingHeap<T, Compare>::Pop(T* out) {
    if (root_ == nullptr) {
        return false;
    }
    Node* old_root = root_;
    if (out != nullptr) {
        *out = std::move(old_root->value);
    }
    // Rebuild the heap from the children of old_root.
    root_ = TwoPassMeld(old_root->child, cmp_);
    delete old_root;
    --size_;
    return true;
}

template<typename T, typename Compare>
void PairingHeap<T, Compare>::Meld(PairingHeap& other) {
    if (this == &other) {
        return;
    }
    root_ = MeldTrees(root_, other.root_, cmp_);
    size_ += other.size_;
    other.root_ = nullptr;
    other.size_ = 0;
}

template<typename T, typename Compare>
void PairingHeap<T, Compare>::DestroySubtree(Node* node) {
    if (node == nullptr) {
        return;
    }
    DestroySubtree(node->child);
    DestroySubtree(node->sibling);
    delete node;
}

template<typename T, typename Compare>
void PairingHeap<T, Compare>::Clear() {
    DestroySubtree(root_);
    root_ = nullptr;
    size_ = 0;
}

}  // namespace mytoydb::lib
