#include "mytoydb/common/containers/node.h"

#include <new>
#include <utility>

#include "mytoydb/common/memory/memory_context.h"

namespace mytoydb::nodes {

// ---------------------------------------------------------------------------
// Value constructors
// ---------------------------------------------------------------------------

Value::Value(int64_t ival) : Node(NodeTag::kInteger), ival_(ival) {}

Value::Value(std::string fval) : Node(NodeTag::kFloat), sval_(std::move(fval)) {}

Value::Value(std::string sval, bool /*is_string*/)
    : Node(NodeTag::kString), sval_(std::move(sval)) {}

Value::Value() : Node(NodeTag::kNull) {}

// ---------------------------------------------------------------------------
// Value::Clone / Value::Equals
// ---------------------------------------------------------------------------

Node* Value::Clone() const {
    void* mem = mytoydb::memory::palloc(sizeof(Value));
    return new (mem) Value(*this);
}

bool Value::Equals(const Node& other) const {
    if (other.GetTag() != GetTag()) {
        return false;
    }
    const Value& o = static_cast<const Value&>(other);
    switch (GetTag()) {
        case NodeTag::kInteger:
            return ival_ == o.ival_;
        case NodeTag::kFloat:
        case NodeTag::kString:
            return sval_ == o.sval_;
        case NodeTag::kNull:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// PostgreSQL-style convenience constructors
// ---------------------------------------------------------------------------

Value* makeInteger(int64_t ival) {
    void* mem = mytoydb::memory::palloc(sizeof(Value));
    return new (mem) Value(ival);
}

Value* makeFloat(std::string fval) {
    void* mem = mytoydb::memory::palloc(sizeof(Value));
    return new (mem) Value(std::move(fval));
}

Value* makeString(std::string sval) {
    void* mem = mytoydb::memory::palloc(sizeof(Value));
    return new (mem) Value(std::move(sval), true);
}

Value* makeNull() {
    void* mem = mytoydb::memory::palloc(sizeof(Value));
    return new (mem) Value();
}

// ---------------------------------------------------------------------------
// copyObject / equal
// ---------------------------------------------------------------------------

Node* copyObject(const Node* node) {
    if (node == nullptr) {
        return nullptr;
    }
    return node->Clone();
}

bool equal(const Node* a, const Node* b) {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a->GetTag() != b->GetTag()) {
        return false;
    }
    return a->Equals(*b);
}

}  // namespace mytoydb::nodes
