#include "pgcpp/common/containers/node.hpp"

#include <new>
#include <utility>

#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::nodes {

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
    return makePallocNode<Value>(*this);
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
    return makePallocNode<Value>(ival);
}

Value* makeFloat(std::string fval) {
    return makePallocNode<Value>(std::move(fval));
}

Value* makeString(std::string sval) {
    return makePallocNode<Value>(std::move(sval), true);
}

Value* makeNull() {
    return makePallocNode<Value>();
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

}  // namespace pgcpp::nodes
