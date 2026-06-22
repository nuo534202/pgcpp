#include "mytoydb/common/containers/list.h"

#include <algorithm>
#include <new>

#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::containers {

// ---------------------------------------------------------------------------
// List method implementations
// ---------------------------------------------------------------------------

void List::Append(void* datum) {
    data_.push_back(datum);
}

void List::Prepend(void* datum) {
    data_.insert(data_.begin(), datum);
}

void* List::Get(std::size_t index) const {
    if (index >= data_.size()) {
        return nullptr;
    }
    return data_[index];
}

void* List::First() const {
    if (data_.empty()) {
        return nullptr;
    }
    return data_.front();
}

void* List::Last() const {
    if (data_.empty()) {
        return nullptr;
    }
    return data_.back();
}

void List::Delete(std::size_t index) {
    if (index < data_.size()) {
        data_.erase(data_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void List::DeletePtr(void* datum) {
    data_.erase(std::remove(data_.begin(), data_.end(), datum), data_.end());
}

void List::Concat(const List& other) {
    data_.insert(data_.end(), other.data_.begin(), other.data_.end());
}

bool List::Member(void* datum) const {
    return std::find(data_.begin(), data_.end(), datum) != data_.end();
}

void List::Reverse() {
    std::reverse(data_.begin(), data_.end());
}

// ---------------------------------------------------------------------------
// PostgreSQL-compatible API (lowercase). These operate on List* allocated
// via palloc. Following PostgreSQL semantics, lappend/lcons on NULL create
// a new list.
// ---------------------------------------------------------------------------

List* newList() {
    void* raw = mytoydb::memory::palloc(sizeof(List));
    return new (raw) List();
}

List* lappend(List* list, void* datum) {
    if (list == nullptr) {
        list = newList();
    }
    list->Append(datum);
    return list;
}

List* lcons(List* list, void* datum) {
    if (list == nullptr) {
        list = newList();
    }
    list->Prepend(datum);
    return list;
}

void* list_nth(const List* list, int index) {
    if (list == nullptr) {
        return nullptr;
    }
    return list->Get(static_cast<std::size_t>(index));
}

void* linitial(const List* list) {
    if (list == nullptr) {
        return nullptr;
    }
    return list->First();
}

void* llast(const List* list) {
    if (list == nullptr) {
        return nullptr;
    }
    return list->Last();
}

int list_length(const List* list) {
    if (list == nullptr) {
        return 0;
    }
    return static_cast<int>(list->Length());
}

List* list_delete_nth_cell(List* list, int index) {
    if (list != nullptr) {
        list->Delete(static_cast<std::size_t>(index));
    }
    return list;
}

List* list_concat(List* list1, const List* list2) {
    if (list1 == nullptr) {
        return const_cast<List*>(list2);
    }
    if (list2 != nullptr) {
        list1->Concat(*list2);
    }
    return list1;
}

bool list_member_ptr(const List* list, void* datum) {
    if (list == nullptr) {
        return false;
    }
    return list->Member(datum);
}

List* list_reverse(List* list) {
    if (list != nullptr) {
        list->Reverse();
    }
    return list;
}

}  // namespace mytoydb::containers
