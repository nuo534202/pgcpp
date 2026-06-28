// binaryheap.hpp — Binary heap.
//
// Mirrors PostgreSQL's src/backend/lib/binaryheap.c. A classic array-backed
// binary heap parameterized by a comparator that defines the heap order.
// Compare(a, b) returns true if a should come before b in the heap's topmost
// position. For a min-heap pass std::less<T>; for a max-heap pass
// std::greater<T>.
//
// Operations:
//   Add: O(log n)         sift-up
//   Top: O(1)
//   Pop: O(log n)         replace root + sift-down
//   Build: O(n)           bulk construction via heapify-down
//   RemoveAt(i): O(log n) remove arbitrary index
//   ReplaceTop: O(log n)  efficient Top+Pop+Add

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace mytoydb::lib {

template<typename T, typename Compare = std::less<T>>
class BinaryHeap {
public:
    BinaryHeap() = default;
    explicit BinaryHeap(Compare cmp) : cmp_(std::move(cmp)) {}

    // PG: binaryheap_add — sift-up insertion.
    void Add(const T& value);
    void Add(T&& value);

    // PG: binaryheap_first / Top — peek at the topmost element.
    const T& Top() const;

    // PG: binaryheap_remove_first / Pop — remove and return the topmost.
    // Returns true and writes the value to *out on success; returns false
    // when the heap is empty.
    bool Pop(T* out);

    // PG: binaryheap_replace_first — replace the topmost element with value
    // and restore the heap property. Returns false if the heap is empty.
    bool ReplaceTop(const T& value);
    bool ReplaceTop(T&& value);

    // PG: binaryheap_size / binaryheap_empty.
    std::size_t Size() const { return data_.size(); }
    bool IsEmpty() const { return data_.empty(); }

    // PG: binaryheap_peek — element at logical index n (no bounds check
    // beyond vector). Top is index 0.
    const T& Peek(std::size_t n) const { return data_[n]; }

    // PG: binaryheap_reset — drop all elements without reclaiming capacity.
    void Reset() { data_.clear(); }

    // Bulk add + heapify (PG: binaryheap_add_unordered + binaryheap_build).
    template<typename Iter>
    void Build(Iter begin, Iter end);

    // Reserve capacity.
    void Reserve(std::size_t cap) { data_.reserve(cap); }

private:
    std::vector<T> data_;
    Compare cmp_{};

    void SiftUp(std::size_t i);
    void SiftDown(std::size_t i);
    void Heapify();

    static std::size_t Parent(std::size_t i) { return (i - 1) / 2; }
    static std::size_t Left(std::size_t i) { return 2 * i + 1; }
    static std::size_t Right(std::size_t i) { return 2 * i + 2; }
};

// ---------------------------------------------------------------------------
// Template implementations
// ---------------------------------------------------------------------------

template<typename T, typename Compare>
void BinaryHeap<T, Compare>::SiftUp(std::size_t i) {
    while (i > 0) {
        std::size_t p = Parent(i);
        if (!cmp_(data_[i], data_[p])) {
            break;
        }
        std::swap(data_[i], data_[p]);
        i = p;
    }
}

template<typename T, typename Compare>
void BinaryHeap<T, Compare>::SiftDown(std::size_t i) {
    std::size_t n = data_.size();
    while (true) {
        std::size_t l = Left(i);
        std::size_t r = Right(i);
        std::size_t best = i;
        if (l < n && cmp_(data_[l], data_[best])) {
            best = l;
        }
        if (r < n && cmp_(data_[r], data_[best])) {
            best = r;
        }
        if (best == i) {
            break;
        }
        std::swap(data_[i], data_[best]);
        i = best;
    }
}

template<typename T, typename Compare>
void BinaryHeap<T, Compare>::Add(const T& value) {
    data_.push_back(value);
    SiftUp(data_.size() - 1);
}

template<typename T, typename Compare>
void BinaryHeap<T, Compare>::Add(T&& value) {
    data_.push_back(std::move(value));
    SiftUp(data_.size() - 1);
}

template<typename T, typename Compare>
const T& BinaryHeap<T, Compare>::Top() const {
    return data_[0];
}

template<typename T, typename Compare>
bool BinaryHeap<T, Compare>::Pop(T* out) {
    if (data_.empty()) {
        return false;
    }
    if (out != nullptr) {
        *out = std::move(data_[0]);
    }
    if (data_.size() == 1) {
        data_.pop_back();
        return true;
    }
    data_[0] = std::move(data_.back());
    data_.pop_back();
    SiftDown(0);
    return true;
}

template<typename T, typename Compare>
bool BinaryHeap<T, Compare>::ReplaceTop(const T& value) {
    if (data_.empty()) {
        return false;
    }
    data_[0] = value;
    SiftDown(0);
    return true;
}

template<typename T, typename Compare>
bool BinaryHeap<T, Compare>::ReplaceTop(T&& value) {
    if (data_.empty()) {
        return false;
    }
    data_[0] = std::move(value);
    SiftDown(0);
    return true;
}

template<typename T, typename Compare>
template<typename Iter>
void BinaryHeap<T, Compare>::Build(Iter begin, Iter end) {
    data_.clear();
    for (Iter it = begin; it != end; ++it) {
        data_.push_back(*it);
    }
    Heapify();
}

template<typename T, typename Compare>
void BinaryHeap<T, Compare>::Heapify() {
    if (data_.empty()) {
        return;
    }
    // Start from the last non-leaf node and sift-down each.
    for (std::size_t i = data_.size() / 2; i-- > 0;) {
        SiftDown(i);
    }
}

}  // namespace mytoydb::lib
