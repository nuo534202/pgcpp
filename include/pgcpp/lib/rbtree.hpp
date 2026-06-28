// rbtree.hpp — Red-black tree.
//
// Mirrors PostgreSQL's src/backend/lib/rbtree.c. The original is an
// intrusive tree that stores caller-allocated RBNode structs and uses a
// comparator function pointer; this C++ port keeps the same algorithm but
// presents a value-based template API: the tree owns its nodes internally
// and exposes Insert / Find / Delete / iteration over stored values.
//
// Standard left-leaning red-black tree invariants:
//   1. Every node is either red or black.
//   2. The root is black.
//   3. Every leaf (NIL) is black (we use a single sentinel).
//   4. Red nodes have no red children.
//   5. Every path from root to leaves has the same number of black nodes.
//
// Insert/Delete/Search are O(log n). Iteration (left-first ascending,
// right-first descending) is O(n). The implementation is recursive-free for
// the rebalancing steps to keep stack usage bounded.

#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace pgcpp::lib {

template<typename T, typename Compare = std::less<T>>
class RBTree {
public:
    RBTree() = default;
    ~RBTree() { Clear(); }

    RBTree(const RBTree&) = delete;
    RBTree& operator=(const RBTree&) = delete;
    RBTree(RBTree&& other) noexcept
        : root_(other.root_), size_(other.size_), cmp_(std::move(other.cmp_)) {
        other.root_ = nullptr;
        other.size_ = 0;
    }
    RBTree& operator=(RBTree&& other) noexcept {
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

    // Insert a value. If an equal value is already present the existing
    // value is left unchanged and the function returns false. Returns true
    // on insertion.
    bool Insert(const T& value);
    bool Insert(T&& value);

    // Look up value. Returns a pointer to the stored copy or nullptr.
    T* Find(const T& value);
    const T* Find(const T& value) const;

    // Delete by value. Returns true if a node was removed.
    bool Delete(const T& value);

    std::size_t Size() const { return size_; }
    bool IsEmpty() const { return size_ == 0; }

    // Walk the tree in ascending key order (left-first). visitor is invoked
    // once per node with a const reference to the value.
    template<typename Visitor>
    void ForeachLeft(Visitor&& visitor) const {
        Foreach(root_, /*left_first=*/true, visitor);
    }

    // Walk in descending key order (right-first).
    template<typename Visitor>
    void ForeachRight(Visitor&& visitor) const {
        Foreach(root_, /*left_first=*/false, visitor);
    }

    // Snapshot to a sorted vector (left-first).
    std::vector<T> ToSortedVector() const;

    void Clear();

private:
    enum class Color : unsigned char { kRed, kBlack };

    struct Node {
        T value;
        Node* left;
        Node* right;
        Node* parent;
        Color color;

        template<typename U>
        explicit Node(U&& v)
            : value(std::forward<U>(v)),
              left(nullptr),
              right(nullptr),
              parent(nullptr),
              color(Color::kRed) {}
    };

    Node* root_ = nullptr;
    std::size_t size_ = 0;
    Compare cmp_{};

    // Helpers
    static bool IsRed(const Node* n) { return n != nullptr && n->color == Color::kRed; }
    static void SetBlack(Node* n) {
        if (n != nullptr)
            n->color = Color::kBlack;
    }
    static void SetRed(Node* n) {
        if (n != nullptr)
            n->color = Color::kRed;
    }

    void RotateLeft(Node* x);
    void RotateRight(Node* x);
    void InsertFixup(Node* z);
    void Transplant(Node* u, Node* v);
    void DeleteFixup(Node* x, Node* xp);
    Node* Min(Node* x) const;
    Node* Max(Node* x) const;

    template<typename Visitor>
    static void Foreach(Node* node, bool left_first, Visitor& visitor) {
        if (node == nullptr) {
            return;
        }
        if (left_first) {
            Foreach(node->left, left_first, visitor);
            visitor(node->value);
            Foreach(node->right, left_first, visitor);
        } else {
            Foreach(node->right, left_first, visitor);
            visitor(node->value);
            Foreach(node->left, left_first, visitor);
        }
    }

    void DestroySubtree(Node* node);
};

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------

template<typename T, typename Compare>
bool RBTree<T, Compare>::Insert(const T& value) {
    Node* z = new Node(value);
    Node* y = nullptr;
    Node* x = root_;
    bool dup = false;
    while (x != nullptr) {
        y = x;
        if (cmp_(z->value, x->value)) {
            x = x->left;
        } else if (cmp_(x->value, z->value)) {
            x = x->right;
        } else {
            dup = true;
            break;
        }
    }
    if (dup) {
        delete z;
        return false;
    }
    z->parent = y;
    if (y == nullptr) {
        root_ = z;
    } else if (cmp_(z->value, y->value)) {
        y->left = z;
    } else {
        y->right = z;
    }
    z->color = Color::kRed;
    InsertFixup(z);
    ++size_;
    return true;
}

template<typename T, typename Compare>
bool RBTree<T, Compare>::Insert(T&& value) {
    Node* z = new Node(std::move(value));
    Node* y = nullptr;
    Node* x = root_;
    bool dup = false;
    while (x != nullptr) {
        y = x;
        if (cmp_(z->value, x->value)) {
            x = x->left;
        } else if (cmp_(x->value, z->value)) {
            x = x->right;
        } else {
            dup = true;
            break;
        }
    }
    if (dup) {
        delete z;
        return false;
    }
    z->parent = y;
    if (y == nullptr) {
        root_ = z;
    } else if (cmp_(z->value, y->value)) {
        y->left = z;
    } else {
        y->right = z;
    }
    z->color = Color::kRed;
    InsertFixup(z);
    ++size_;
    return true;
}

template<typename T, typename Compare>
T* RBTree<T, Compare>::Find(const T& value) {
    Node* x = root_;
    while (x != nullptr) {
        if (cmp_(value, x->value)) {
            x = x->left;
        } else if (cmp_(x->value, value)) {
            x = x->right;
        } else {
            return &x->value;
        }
    }
    return nullptr;
}

template<typename T, typename Compare>
const T* RBTree<T, Compare>::Find(const T& value) const {
    Node* x = root_;
    while (x != nullptr) {
        if (cmp_(value, x->value)) {
            x = x->left;
        } else if (cmp_(x->value, value)) {
            x = x->right;
        } else {
            return &x->value;
        }
    }
    return nullptr;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::RotateLeft(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left != nullptr) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nullptr) {
        root_ = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::RotateRight(Node* x) {
    Node* y = x->left;
    x->left = y->right;
    if (y->right != nullptr) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nullptr) {
        root_ = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::InsertFixup(Node* z) {
    while (z != nullptr && z->parent != nullptr && IsRed(z->parent)) {
        if (z->parent == z->parent->parent->left) {
            Node* y = z->parent->parent->right;
            if (IsRed(y)) {
                SetBlack(z->parent);
                SetBlack(y);
                SetRed(z->parent->parent);
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    RotateLeft(z);
                }
                SetBlack(z->parent);
                SetRed(z->parent->parent);
                RotateRight(z->parent->parent);
            }
        } else {
            Node* y = z->parent->parent->left;
            if (IsRed(y)) {
                SetBlack(z->parent);
                SetBlack(y);
                SetRed(z->parent->parent);
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    RotateRight(z);
                }
                SetBlack(z->parent);
                SetRed(z->parent->parent);
                RotateLeft(z->parent->parent);
            }
        }
    }
    SetBlack(root_);
}

template<typename T, typename Compare>
void RBTree<T, Compare>::Transplant(Node* u, Node* v) {
    if (u->parent == nullptr) {
        root_ = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    if (v != nullptr) {
        v->parent = u->parent;
    }
}

template<typename T, typename Compare>
typename RBTree<T, Compare>::Node* RBTree<T, Compare>::Min(Node* x) const {
    while (x != nullptr && x->left != nullptr) {
        x = x->left;
    }
    return x;
}

template<typename T, typename Compare>
typename RBTree<T, Compare>::Node* RBTree<T, Compare>::Max(Node* x) const {
    while (x != nullptr && x->right != nullptr) {
        x = x->right;
    }
    return x;
}

template<typename T, typename Compare>
bool RBTree<T, Compare>::Delete(const T& value) {
    Node* z = root_;
    while (z != nullptr) {
        if (cmp_(value, z->value)) {
            z = z->left;
        } else if (cmp_(z->value, value)) {
            z = z->right;
        } else {
            break;
        }
    }
    if (z == nullptr) {
        return false;
    }

    Node* y = z;
    Node* x = nullptr;
    Node* xp = nullptr;  // parent of x after transplant
    Color y_original_color = y->color;
    if (z->left == nullptr) {
        x = z->right;
        xp = z->parent;
        Transplant(z, z->right);
    } else if (z->right == nullptr) {
        x = z->left;
        xp = z->parent;
        Transplant(z, z->left);
    } else {
        y = Min(z->right);
        y_original_color = y->color;
        x = y->right;
        if (y->parent == z) {
            xp = y;
        } else {
            xp = y->parent;
            Transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        Transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    z->left = nullptr;
    z->right = nullptr;
    z->parent = nullptr;
    delete z;
    --size_;
    if (y_original_color == Color::kBlack) {
        DeleteFixup(x, xp);
    }
    return true;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::DeleteFixup(Node* x, Node* xp) {
    while (x != root_ && !IsRed(x)) {
        if (x == xp->left) {
            Node* w = xp->right;
            if (IsRed(w)) {
                SetBlack(w);
                SetRed(xp);
                RotateLeft(xp);
                w = xp->right;
            }
            if (w != nullptr && !IsRed(w->left) && !IsRed(w->right)) {
                SetRed(w);
                x = xp;
                xp = x->parent;
            } else {
                if (w != nullptr && !IsRed(w->right)) {
                    if (w->left != nullptr)
                        SetBlack(w->left);
                    SetRed(w);
                    RotateRight(w);
                    w = xp->right;
                }
                if (w != nullptr) {
                    w->color = xp->color;
                    SetBlack(xp);
                    if (w->right != nullptr)
                        SetBlack(w->right);
                }
                RotateLeft(xp);
                x = root_;
                xp = nullptr;
            }
        } else {
            Node* w = xp->left;
            if (IsRed(w)) {
                SetBlack(w);
                SetRed(xp);
                RotateRight(xp);
                w = xp->left;
            }
            if (w != nullptr && !IsRed(w->right) && !IsRed(w->left)) {
                SetRed(w);
                x = xp;
                xp = x->parent;
            } else {
                if (w != nullptr && !IsRed(w->left)) {
                    if (w->right != nullptr)
                        SetBlack(w->right);
                    SetRed(w);
                    RotateLeft(w);
                    w = xp->left;
                }
                if (w != nullptr) {
                    w->color = xp->color;
                    SetBlack(xp);
                    if (w->left != nullptr)
                        SetBlack(w->left);
                }
                RotateRight(xp);
                x = root_;
                xp = nullptr;
            }
        }
    }
    SetBlack(x);
}

template<typename T, typename Compare>
std::vector<T> RBTree<T, Compare>::ToSortedVector() const {
    std::vector<T> out;
    out.reserve(size_);
    ForeachLeft([&out](const T& v) { out.push_back(v); });
    return out;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::DestroySubtree(Node* node) {
    if (node == nullptr) {
        return;
    }
    DestroySubtree(node->left);
    DestroySubtree(node->right);
    delete node;
}

template<typename T, typename Compare>
void RBTree<T, Compare>::Clear() {
    DestroySubtree(root_);
    root_ = nullptr;
    size_ = 0;
}

}  // namespace pgcpp::lib
