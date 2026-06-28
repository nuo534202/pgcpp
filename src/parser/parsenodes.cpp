// parsenodes.cpp — Clone() and Equals() implementations for parse node types.
//
// Converted from PostgreSQL 15's parse tree nodes. Each Clone() uses palloc
// for the node allocation (matching the memory-context model) and deep-copies
// all Node* and std::vector<Node*> fields. Each Equals() compares all fields.

#include "pgcpp/parser/parsenodes.hpp"

#include <new>
#include <utility>

#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/memory/memory_context.hpp"

namespace pgcpp::parser {
using pgcpp::nodes::makePallocNode;

using pgcpp::nodes::copyObject;
using pgcpp::nodes::equal;
using pgcpp::nodes::Node;

namespace {

// Deep-copy a vector of Node* using copyObject on each element.
std::vector<Node*> CloneVec(const std::vector<Node*>& vec) {
    std::vector<Node*> result;
    result.reserve(vec.size());
    for (Node* n : vec) {
        result.push_back(copyObject(n));
    }
    return result;
}

// Deep-copy a Node* (returns nullptr if input is nullptr).
Node* CloneNode(Node* n) {
    return n == nullptr ? nullptr : copyObject(n);
}

// Compare two Node* for deep equality (both nullptr => equal).
bool EqNode(const Node* a, const Node* b) {
    return equal(a, b);
}

// Compare two vectors of Node* for deep equality.
bool EqVec(const std::vector<Node*>& a, const std::vector<Node*>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!EqNode(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ===========================================================================
// Type name and column reference nodes
// ===========================================================================

Node* TypeName::Clone() const {
    auto* copy = makePallocNode<TypeName>(*this);
    copy->names = CloneVec(names);
    copy->typmods = CloneVec(typmods);
    copy->array_bounds = CloneVec(array_bounds);
    return copy;
}

bool TypeName::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TypeName&>(other);
    return EqVec(names, o.names) && type_oid == o.type_oid && setof == o.setof &&
           pct_type == o.pct_type && EqVec(typmods, o.typmods) && typemod == o.typemod &&
           EqVec(array_bounds, o.array_bounds) && location == o.location;
}

Node* ColumnRef::Clone() const {
    auto* copy = makePallocNode<ColumnRef>(*this);
    copy->fields = CloneVec(fields);
    return copy;
}

bool ColumnRef::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ColumnRef&>(other);
    return EqVec(fields, o.fields) && location == o.location;
}

Node* ParamRef::Clone() const {
    return makePallocNode<ParamRef>(*this);
}

bool ParamRef::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ParamRef&>(other);
    return number == o.number && location == o.location;
}

// ===========================================================================
// Expression nodes
// ===========================================================================

Node* AExpr::Clone() const {
    auto* copy = makePallocNode<AExpr>(*this);
    copy->name = CloneVec(name);
    copy->lexpr = CloneNode(lexpr);
    copy->rexpr = CloneNode(rexpr);
    return copy;
}

bool AExpr::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AExpr&>(other);
    return kind == o.kind && EqVec(name, o.name) && EqNode(lexpr, o.lexpr) &&
           EqNode(rexpr, o.rexpr) && location == o.location;
}

Node* AConst::Clone() const {
    auto* copy = makePallocNode<AConst>(*this);
    copy->val = CloneNode(val);
    return copy;
}

bool AConst::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AConst&>(other);
    return isnull == o.isnull && isbool == o.isbool && location == o.location &&
           (isnull || EqNode(val, o.val));
}

Node* TypeCast::Clone() const {
    auto* copy = makePallocNode<TypeCast>(*this);
    copy->arg = CloneNode(arg);
    copy->type_name = static_cast<TypeName*>(CloneNode(type_name));
    return copy;
}

bool TypeCast::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TypeCast&>(other);
    return EqNode(arg, o.arg) && EqNode(type_name, o.type_name) && location == o.location;
}

Node* CollateClause::Clone() const {
    auto* copy = makePallocNode<CollateClause>(*this);
    copy->arg = CloneNode(arg);
    copy->collname = CloneVec(collname);
    return copy;
}

bool CollateClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CollateClause&>(other);
    return EqNode(arg, o.arg) && EqVec(collname, o.collname) && location == o.location;
}

Node* RoleSpec::Clone() const {
    return makePallocNode<RoleSpec>(*this);
}

bool RoleSpec::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RoleSpec&>(other);
    return roletype == o.roletype && rolename == o.rolename && location == o.location;
}

Node* FuncCall::Clone() const {
    auto* copy = makePallocNode<FuncCall>(*this);
    copy->funcname = CloneVec(funcname);
    copy->args = CloneVec(args);
    copy->agg_order = CloneVec(agg_order);
    copy->agg_filter = CloneNode(agg_filter);
    copy->over = static_cast<WindowDef*>(CloneNode(over));
    return copy;
}

bool FuncCall::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const FuncCall&>(other);
    return EqVec(funcname, o.funcname) && EqVec(args, o.args) && EqVec(agg_order, o.agg_order) &&
           EqNode(agg_filter, o.agg_filter) && EqNode(over, o.over) &&
           agg_within_group == o.agg_within_group && agg_star == o.agg_star &&
           agg_distinct == o.agg_distinct && func_variadic == o.func_variadic &&
           funcformat == o.funcformat && location == o.location;
}

Node* AStar::Clone() const {
    return makePallocNode<AStar>(*this);
}

bool AStar::Equals(const Node& other) const {
    return other.GetTag() == GetTag();
}

Node* AIndices::Clone() const {
    auto* copy = makePallocNode<AIndices>(*this);
    copy->lidx = CloneNode(lidx);
    copy->uidx = CloneNode(uidx);
    return copy;
}

bool AIndices::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AIndices&>(other);
    return is_slice == o.is_slice && EqNode(lidx, o.lidx) && EqNode(uidx, o.uidx);
}

Node* AIndirection::Clone() const {
    auto* copy = makePallocNode<AIndirection>(*this);
    copy->arg = CloneNode(arg);
    copy->indirection = CloneVec(indirection);
    return copy;
}

bool AIndirection::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AIndirection&>(other);
    return EqNode(arg, o.arg) && EqVec(indirection, o.indirection);
}

Node* AArrayExpr::Clone() const {
    auto* copy = makePallocNode<AArrayExpr>(*this);
    copy->elements = CloneVec(elements);
    return copy;
}

bool AArrayExpr::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AArrayExpr&>(other);
    return EqVec(elements, o.elements) && location == o.location;
}

Node* ResTarget::Clone() const {
    auto* copy = makePallocNode<ResTarget>(*this);
    copy->indirection = CloneVec(indirection);
    copy->val = CloneNode(val);
    return copy;
}

bool ResTarget::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ResTarget&>(other);
    return name == o.name && EqVec(indirection, o.indirection) && EqNode(val, o.val) &&
           location == o.location;
}

Node* MultiAssignRef::Clone() const {
    auto* copy = makePallocNode<MultiAssignRef>(*this);
    copy->source = CloneNode(source);
    return copy;
}

bool MultiAssignRef::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const MultiAssignRef&>(other);
    return EqNode(source, o.source) && colno == o.colno && ncolumns == o.ncolumns;
}

Node* SortBy::Clone() const {
    auto* copy = makePallocNode<SortBy>(*this);
    copy->node = CloneNode(node);
    copy->use_op = CloneVec(use_op);
    return copy;
}

bool SortBy::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const SortBy&>(other);
    return EqNode(node, o.node) && sortby_dir == o.sortby_dir && sortby_nulls == o.sortby_nulls &&
           EqVec(use_op, o.use_op) && location == o.location;
}

Node* WindowDef::Clone() const {
    auto* copy = makePallocNode<WindowDef>(*this);
    copy->partition_clause = CloneVec(partition_clause);
    copy->order_clause = CloneVec(order_clause);
    copy->start_offset = CloneNode(start_offset);
    copy->end_offset = CloneNode(end_offset);
    return copy;
}

bool WindowDef::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const WindowDef&>(other);
    return name == o.name && refname == o.refname && EqVec(partition_clause, o.partition_clause) &&
           EqVec(order_clause, o.order_clause) && frame_options == o.frame_options &&
           EqNode(start_offset, o.start_offset) && EqNode(end_offset, o.end_offset) &&
           location == o.location;
}

// ===========================================================================
// Range table node types
// ===========================================================================

Node* RangeSubselect::Clone() const {
    auto* copy = makePallocNode<RangeSubselect>(*this);
    copy->subquery = CloneNode(subquery);
    copy->alias = static_cast<Alias*>(CloneNode(alias));
    return copy;
}

bool RangeSubselect::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RangeSubselect&>(other);
    return lateral == o.lateral && EqNode(subquery, o.subquery) && EqNode(alias, o.alias);
}

Node* RangeFunction::Clone() const {
    auto* copy = makePallocNode<RangeFunction>(*this);
    copy->functions = CloneVec(functions);
    copy->alias = static_cast<Alias*>(CloneNode(alias));
    copy->coldeflist = CloneVec(coldeflist);
    return copy;
}

bool RangeFunction::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RangeFunction&>(other);
    return lateral == o.lateral && ordinality == o.ordinality && is_rowsfrom == o.is_rowsfrom &&
           EqVec(functions, o.functions) && EqNode(alias, o.alias) &&
           EqVec(coldeflist, o.coldeflist);
}

Node* ColumnDef::Clone() const {
    auto* copy = makePallocNode<ColumnDef>(*this);
    copy->type_name = static_cast<TypeName*>(CloneNode(type_name));
    copy->raw_default = CloneNode(raw_default);
    copy->cooked_default = CloneNode(cooked_default);
    copy->identity_sequence = static_cast<RangeVar*>(CloneNode(identity_sequence));
    copy->coll_clause = static_cast<CollateClause*>(CloneNode(coll_clause));
    copy->constraints = CloneVec(constraints);
    copy->fdwoptions = CloneVec(fdwoptions);
    return copy;
}

bool ColumnDef::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ColumnDef&>(other);
    return colname == o.colname && EqNode(type_name, o.type_name) && compression == o.compression &&
           inhcount == o.inhcount && is_local == o.is_local && is_not_null == o.is_not_null &&
           is_from_type == o.is_from_type && storage == o.storage &&
           EqNode(raw_default, o.raw_default) && EqNode(cooked_default, o.cooked_default) &&
           identity == o.identity && EqNode(identity_sequence, o.identity_sequence) &&
           generated == o.generated && EqNode(coll_clause, o.coll_clause) &&
           coll_oid == o.coll_oid && EqVec(constraints, o.constraints) &&
           EqVec(fdwoptions, o.fdwoptions) && location == o.location;
}

Node* IndexElem::Clone() const {
    auto* copy = makePallocNode<IndexElem>(*this);
    copy->expr = CloneNode(expr);
    copy->collation = CloneVec(collation);
    copy->opclass = CloneVec(opclass);
    copy->opclassopts = CloneVec(opclassopts);
    return copy;
}

bool IndexElem::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const IndexElem&>(other);
    return name == o.name && EqNode(expr, o.expr) && indexcolname == o.indexcolname &&
           EqVec(collation, o.collation) && EqVec(opclass, o.opclass) &&
           EqVec(opclassopts, o.opclassopts) && ordering == o.ordering &&
           nulls_ordering == o.nulls_ordering;
}

Node* DefElem::Clone() const {
    auto* copy = makePallocNode<DefElem>(*this);
    copy->arg = CloneNode(arg);
    return copy;
}

bool DefElem::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DefElem&>(other);
    return defnamespace == o.defnamespace && defname == o.defname && EqNode(arg, o.arg) &&
           defaction == o.defaction && location == o.location;
}

Node* LockingClause::Clone() const {
    auto* copy = makePallocNode<LockingClause>(*this);
    copy->locked_rels = CloneVec(locked_rels);
    return copy;
}

bool LockingClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const LockingClause&>(other);
    return EqVec(locked_rels, o.locked_rels) && strength == o.strength &&
           wait_policy == o.wait_policy;
}

Node* XmlSerialize::Clone() const {
    auto* copy = makePallocNode<XmlSerialize>(*this);
    copy->expr = CloneNode(expr);
    copy->type_name = static_cast<TypeName*>(CloneNode(type_name));
    return copy;
}

bool XmlSerialize::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const XmlSerialize&>(other);
    return xmloption == o.xmloption && EqNode(expr, o.expr) && EqNode(type_name, o.type_name) &&
           location == o.location;
}

// ===========================================================================
// Partitioning node types
// ===========================================================================

Node* PartitionElem::Clone() const {
    auto* copy = makePallocNode<PartitionElem>(*this);
    copy->expr = CloneNode(expr);
    copy->collation = CloneVec(collation);
    copy->opclass = CloneVec(opclass);
    return copy;
}

bool PartitionElem::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PartitionElem&>(other);
    return name == o.name && EqNode(expr, o.expr) && EqVec(collation, o.collation) &&
           EqVec(opclass, o.opclass) && location == o.location;
}

Node* PartitionSpec::Clone() const {
    auto* copy = makePallocNode<PartitionSpec>(*this);
    copy->part_params = CloneVec(part_params);
    return copy;
}

bool PartitionSpec::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PartitionSpec&>(other);
    return strategy == o.strategy && EqVec(part_params, o.part_params) && location == o.location;
}

Node* PartitionBoundSpec::Clone() const {
    auto* copy = makePallocNode<PartitionBoundSpec>(*this);
    copy->listdatums = CloneVec(listdatums);
    copy->lowerdatums = CloneVec(lowerdatums);
    copy->upperdatums = CloneVec(upperdatums);
    return copy;
}

bool PartitionBoundSpec::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PartitionBoundSpec&>(other);
    return strategy == o.strategy && is_default == o.is_default && modulus == o.modulus &&
           remainder == o.remainder && EqVec(listdatums, o.listdatums) &&
           EqVec(lowerdatums, o.lowerdatums) && EqVec(upperdatums, o.upperdatums) &&
           location == o.location;
}

Node* PartitionRangeDatum::Clone() const {
    auto* copy = makePallocNode<PartitionRangeDatum>(*this);
    copy->value = CloneNode(value);
    return copy;
}

bool PartitionRangeDatum::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PartitionRangeDatum&>(other);
    return kind == o.kind && EqNode(value, o.value) && location == o.location;
}

Node* PartitionCmd::Clone() const {
    auto* copy = makePallocNode<PartitionCmd>(*this);
    copy->name = static_cast<RangeVar*>(CloneNode(name));
    copy->bound = static_cast<PartitionBoundSpec*>(CloneNode(bound));
    return copy;
}

bool PartitionCmd::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PartitionCmd&>(other);
    return EqNode(name, o.name) && EqNode(bound, o.bound) && concurrent == o.concurrent;
}

// ===========================================================================
// Range table entry and related types
// ===========================================================================

Node* RangeTblEntry::Clone() const {
    auto* copy = makePallocNode<RangeTblEntry>(*this);
    copy->tablesample = static_cast<TableSampleClause*>(CloneNode(tablesample));
    copy->subquery = static_cast<Query*>(CloneNode(subquery));
    copy->joinaliasvars = CloneVec(joinaliasvars);
    copy->joinleftcols = CloneVec(joinleftcols);
    copy->joinrightcols = CloneVec(joinrightcols);
    copy->join_using_alias = static_cast<Alias*>(CloneNode(join_using_alias));
    copy->functions = CloneVec(functions);
    copy->tablefunc = CloneNode(tablefunc);
    copy->values_lists = CloneVec(values_lists);
    copy->coltypes = CloneVec(coltypes);
    copy->coltypmods = CloneVec(coltypmods);
    copy->colcollations = CloneVec(colcollations);
    copy->alias = static_cast<Alias*>(CloneNode(alias));
    copy->eref = static_cast<Alias*>(CloneNode(eref));
    copy->security_quals = CloneVec(security_quals);
    return copy;
}

bool RangeTblEntry::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RangeTblEntry&>(other);
    return rtekind == o.rtekind && relid == o.relid && relkind == o.relkind &&
           rellockmode == o.rellockmode && EqNode(tablesample, o.tablesample) &&
           EqNode(subquery, o.subquery) && security_barrier == o.security_barrier &&
           jointype == o.jointype && joinmergedcols == o.joinmergedcols &&
           EqVec(joinaliasvars, o.joinaliasvars) && EqVec(joinleftcols, o.joinleftcols) &&
           EqVec(joinrightcols, o.joinrightcols) && EqNode(join_using_alias, o.join_using_alias) &&
           EqVec(functions, o.functions) && funcordinality == o.funcordinality &&
           EqNode(tablefunc, o.tablefunc) && EqVec(values_lists, o.values_lists) &&
           ctename == o.ctename && ctelevelsup == o.ctelevelsup &&
           self_reference == o.self_reference && EqVec(coltypes, o.coltypes) &&
           EqVec(coltypmods, o.coltypmods) && EqVec(colcollations, o.colcollations) &&
           enrname == o.enrname && enrtuples == o.enrtuples && EqNode(alias, o.alias) &&
           EqNode(eref, o.eref) && lateral == o.lateral && inh == o.inh &&
           in_from_cl == o.in_from_cl && required_perms == o.required_perms &&
           check_as_user == o.check_as_user && selected_cols == o.selected_cols &&
           inserted_cols == o.inserted_cols && updated_cols == o.updated_cols &&
           extra_updated_cols == o.extra_updated_cols && EqVec(security_quals, o.security_quals);
}

Node* RangeTblFunction::Clone() const {
    auto* copy = makePallocNode<RangeTblFunction>(*this);
    copy->funcexpr = CloneNode(funcexpr);
    copy->funccolnames = CloneVec(funccolnames);
    copy->funccoltypes = CloneVec(funccoltypes);
    copy->funccoltypmods = CloneVec(funccoltypmods);
    copy->funccolcollations = CloneVec(funccolcollations);
    return copy;
}

bool RangeTblFunction::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RangeTblFunction&>(other);
    return EqNode(funcexpr, o.funcexpr) && funccolcount == o.funccolcount &&
           EqVec(funccolnames, o.funccolnames) && EqVec(funccoltypes, o.funccoltypes) &&
           EqVec(funccoltypmods, o.funccoltypmods) &&
           EqVec(funccolcollations, o.funccolcollations) && funcparams == o.funcparams;
}

Node* TableSampleClause::Clone() const {
    auto* copy = makePallocNode<TableSampleClause>(*this);
    copy->args = CloneVec(args);
    copy->repeatable = CloneNode(repeatable);
    return copy;
}

bool TableSampleClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TableSampleClause&>(other);
    return tsmhandler == o.tsmhandler && EqVec(args, o.args) && EqNode(repeatable, o.repeatable);
}

Node* WithCheckOption::Clone() const {
    auto* copy = makePallocNode<WithCheckOption>(*this);
    copy->qual = CloneNode(qual);
    return copy;
}

bool WithCheckOption::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const WithCheckOption&>(other);
    return kind == o.kind && relname == o.relname && polname == o.polname && EqNode(qual, o.qual) &&
           cascaded == o.cascaded;
}

// ===========================================================================
// Sort/group/window clause types
// ===========================================================================

Node* SortGroupClause::Clone() const {
    return makePallocNode<SortGroupClause>(*this);
}

bool SortGroupClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const SortGroupClause&>(other);
    return tle_sort_group_ref == o.tle_sort_group_ref && eqop == o.eqop && sortop == o.sortop &&
           nulls_first == o.nulls_first && hashable == o.hashable;
}

Node* GroupingSet::Clone() const {
    auto* copy = makePallocNode<GroupingSet>(*this);
    copy->content = CloneVec(content);
    return copy;
}

bool GroupingSet::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const GroupingSet&>(other);
    return kind == o.kind && EqVec(content, o.content) && location == o.location;
}

Node* WindowClause::Clone() const {
    auto* copy = makePallocNode<WindowClause>(*this);
    copy->partition_clause = CloneVec(partition_clause);
    copy->order_clause = CloneVec(order_clause);
    copy->start_offset = CloneNode(start_offset);
    copy->end_offset = CloneNode(end_offset);
    copy->run_condition = CloneVec(run_condition);
    return copy;
}

bool WindowClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const WindowClause&>(other);
    return name == o.name && refname == o.refname && EqVec(partition_clause, o.partition_clause) &&
           EqVec(order_clause, o.order_clause) && frame_options == o.frame_options &&
           EqNode(start_offset, o.start_offset) && EqNode(end_offset, o.end_offset) &&
           EqVec(run_condition, o.run_condition) && start_in_range_func == o.start_in_range_func &&
           end_in_range_func == o.end_in_range_func && in_range_coll == o.in_range_coll &&
           in_range_asc == o.in_range_asc && in_range_nulls_first == o.in_range_nulls_first &&
           winref == o.winref && copied_order == o.copied_order;
}

Node* RowMarkClause::Clone() const {
    return makePallocNode<RowMarkClause>(*this);
}

bool RowMarkClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RowMarkClause&>(other);
    return rti == o.rti && strength == o.strength && wait_policy == o.wait_policy &&
           pushed_down == o.pushed_down;
}

// ===========================================================================
// WITH clause and conflict clause types
// ===========================================================================

Node* WithClause::Clone() const {
    auto* copy = makePallocNode<WithClause>(*this);
    copy->ctes = CloneVec(ctes);
    return copy;
}

bool WithClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const WithClause&>(other);
    return EqVec(ctes, o.ctes) && recursive == o.recursive && location == o.location;
}

Node* InferClause::Clone() const {
    auto* copy = makePallocNode<InferClause>(*this);
    copy->index_elems = CloneVec(index_elems);
    copy->where_clause = CloneNode(where_clause);
    return copy;
}

bool InferClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const InferClause&>(other);
    return EqVec(index_elems, o.index_elems) && EqNode(where_clause, o.where_clause) &&
           conname == o.conname && location == o.location;
}

Node* OnConflictClause::Clone() const {
    auto* copy = makePallocNode<OnConflictClause>(*this);
    copy->infer = static_cast<InferClause*>(CloneNode(infer));
    copy->target_list = CloneVec(target_list);
    copy->where_clause = CloneNode(where_clause);
    return copy;
}

bool OnConflictClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const OnConflictClause&>(other);
    return action == o.action && EqNode(infer, o.infer) && EqVec(target_list, o.target_list) &&
           EqNode(where_clause, o.where_clause) && location == o.location;
}

Node* CommonTableExpr::Clone() const {
    auto* copy = makePallocNode<CommonTableExpr>(*this);
    copy->aliascolnames = CloneVec(aliascolnames);
    copy->ctequery = CloneNode(ctequery);
    copy->search_clause = CloneNode(search_clause);
    copy->cycle_clause = CloneNode(cycle_clause);
    copy->ctecolnames = CloneVec(ctecolnames);
    copy->ctecoltypes = CloneVec(ctecoltypes);
    copy->ctecoltypmods = CloneVec(ctecoltypmods);
    copy->ctecolcollations = CloneVec(ctecolcollations);
    return copy;
}

bool CommonTableExpr::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CommonTableExpr&>(other);
    return ctename == o.ctename && EqVec(aliascolnames, o.aliascolnames) &&
           ctematerialized == o.ctematerialized && EqNode(ctequery, o.ctequery) &&
           EqNode(search_clause, o.search_clause) && EqNode(cycle_clause, o.cycle_clause) &&
           location == o.location && cterecursive == o.cterecursive &&
           cterefcount == o.cterefcount && EqVec(ctecolnames, o.ctecolnames) &&
           EqVec(ctecoltypes, o.ctecoltypes) && EqVec(ctecoltypmods, o.ctecoltypmods) &&
           EqVec(ctecolcollations, o.ctecolcollations);
}

// ===========================================================================
// MERGE statement types
// ===========================================================================

Node* MergeWhenClause::Clone() const {
    auto* copy = makePallocNode<MergeWhenClause>(*this);
    copy->condition = CloneNode(condition);
    copy->target_list = CloneVec(target_list);
    copy->values = CloneVec(values);
    return copy;
}

bool MergeWhenClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const MergeWhenClause&>(other);
    return matched == o.matched && command_type == o.command_type &&
           override_kind == o.override_kind && EqNode(condition, o.condition) &&
           EqVec(target_list, o.target_list) && EqVec(values, o.values);
}

Node* MergeAction::Clone() const {
    auto* copy = makePallocNode<MergeAction>(*this);
    copy->qual = CloneNode(qual);
    copy->target_list = CloneVec(target_list);
    copy->update_colnos = CloneVec(update_colnos);
    return copy;
}

bool MergeAction::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const MergeAction&>(other);
    return matched == o.matched && command_type == o.command_type &&
           override_kind == o.override_kind && EqNode(qual, o.qual) &&
           EqVec(target_list, o.target_list) && EqVec(update_colnos, o.update_colnos);
}

Node* TriggerTransition::Clone() const {
    return makePallocNode<TriggerTransition>(*this);
}

bool TriggerTransition::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TriggerTransition&>(other);
    return name == o.name && is_new == o.is_new && is_table == o.is_table;
}

// ===========================================================================
// Privilege and role types
// ===========================================================================

Node* AccessPriv::Clone() const {
    auto* copy = makePallocNode<AccessPriv>(*this);
    copy->cols = CloneVec(cols);
    return copy;
}

bool AccessPriv::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AccessPriv&>(other);
    return priv_name == o.priv_name && EqVec(cols, o.cols);
}

// ===========================================================================
// Alias and range var types
// ===========================================================================

Node* Alias::Clone() const {
    auto* copy = makePallocNode<Alias>(*this);
    copy->colnames = CloneVec(colnames);
    return copy;
}

bool Alias::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const Alias&>(other);
    return aliasname == o.aliasname && EqVec(colnames, o.colnames);
}

Node* RangeVar::Clone() const {
    auto* copy = makePallocNode<RangeVar>(*this);
    copy->alias = static_cast<Alias*>(CloneNode(alias));
    return copy;
}

bool RangeVar::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RangeVar&>(other);
    return catalogname == o.catalogname && schemaname == o.schemaname && relname == o.relname &&
           inh == o.inh && relpersistence == o.relpersistence && EqNode(alias, o.alias) &&
           location == o.location;
}

Node* IntoClause::Clone() const {
    auto* copy = makePallocNode<IntoClause>(*this);
    copy->rel = static_cast<RangeVar*>(CloneNode(rel));
    copy->col_names = CloneVec(col_names);
    copy->options = CloneVec(options);
    copy->view_query = CloneNode(view_query);
    return copy;
}

bool IntoClause::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const IntoClause&>(other);
    return EqNode(rel, o.rel) && EqVec(col_names, o.col_names) &&
           access_method == o.access_method && EqVec(options, o.options) &&
           on_commit == o.on_commit && table_space_name == o.table_space_name &&
           EqNode(view_query, o.view_query) && skip_data == o.skip_data;
}

// ===========================================================================
// Statement nodes
// ===========================================================================

Node* RawStmt::Clone() const {
    auto* copy = makePallocNode<RawStmt>(*this);
    copy->stmt = CloneNode(stmt);
    return copy;
}

bool RawStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RawStmt&>(other);
    return EqNode(stmt, o.stmt) && stmt_location == o.stmt_location && stmt_len == o.stmt_len;
}

Node* InsertStmt::Clone() const {
    auto* copy = makePallocNode<InsertStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->cols = CloneVec(cols);
    copy->select_stmt = CloneNode(select_stmt);
    copy->on_conflict_clause = static_cast<OnConflictClause*>(CloneNode(on_conflict_clause));
    copy->returning_list = CloneVec(returning_list);
    copy->with_clause = static_cast<WithClause*>(CloneNode(with_clause));
    return copy;
}

bool InsertStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const InsertStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(cols, o.cols) &&
           EqNode(select_stmt, o.select_stmt) && EqNode(on_conflict_clause, o.on_conflict_clause) &&
           EqVec(returning_list, o.returning_list) && EqNode(with_clause, o.with_clause) &&
           override_kind == o.override_kind;
}

Node* DeleteStmt::Clone() const {
    auto* copy = makePallocNode<DeleteStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->using_clause = CloneVec(using_clause);
    copy->where_clause = CloneNode(where_clause);
    copy->returning_list = CloneVec(returning_list);
    copy->with_clause = static_cast<WithClause*>(CloneNode(with_clause));
    return copy;
}

bool DeleteStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DeleteStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(using_clause, o.using_clause) &&
           EqNode(where_clause, o.where_clause) && EqVec(returning_list, o.returning_list) &&
           EqNode(with_clause, o.with_clause);
}

Node* UpdateStmt::Clone() const {
    auto* copy = makePallocNode<UpdateStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->target_list = CloneVec(target_list);
    copy->where_clause = CloneNode(where_clause);
    copy->from_clause = CloneVec(from_clause);
    copy->returning_list = CloneVec(returning_list);
    copy->with_clause = static_cast<WithClause*>(CloneNode(with_clause));
    return copy;
}

bool UpdateStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const UpdateStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(target_list, o.target_list) &&
           EqNode(where_clause, o.where_clause) && EqVec(from_clause, o.from_clause) &&
           EqVec(returning_list, o.returning_list) && EqNode(with_clause, o.with_clause);
}

Node* SelectStmt::Clone() const {
    auto* copy = makePallocNode<SelectStmt>(*this);
    copy->distinct_clause = CloneVec(distinct_clause);
    copy->into_clause = static_cast<IntoClause*>(CloneNode(into_clause));
    copy->target_list = CloneVec(target_list);
    copy->from_clause = CloneVec(from_clause);
    copy->where_clause = CloneNode(where_clause);
    copy->group_clause = CloneVec(group_clause);
    copy->having_clause = CloneNode(having_clause);
    copy->window_clause = CloneVec(window_clause);
    // values_lists is a vector of vectors — deep-copy each inner list.
    copy->values_lists.clear();
    for (const auto& inner : values_lists) {
        copy->values_lists.push_back(CloneVec(inner));
    }
    copy->sort_clause = CloneVec(sort_clause);
    copy->limit_offset = CloneNode(limit_offset);
    copy->limit_count = CloneNode(limit_count);
    copy->locking_clause = CloneVec(locking_clause);
    copy->with_clause = static_cast<WithClause*>(CloneNode(with_clause));
    copy->larg = static_cast<SelectStmt*>(CloneNode(larg));
    copy->rarg = static_cast<SelectStmt*>(CloneNode(rarg));
    return copy;
}

bool SelectStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const SelectStmt&>(other);
    return EqVec(distinct_clause, o.distinct_clause) && EqNode(into_clause, o.into_clause) &&
           EqVec(target_list, o.target_list) && EqVec(from_clause, o.from_clause) &&
           EqNode(where_clause, o.where_clause) && EqVec(group_clause, o.group_clause) &&
           group_distinct == o.group_distinct && EqNode(having_clause, o.having_clause) &&
           EqVec(window_clause, o.window_clause) &&
           // values_lists is a vector of vectors — compare each inner list.
           [&]() {
               if (values_lists.size() != o.values_lists.size())
                   return false;
               for (std::size_t i = 0; i < values_lists.size(); ++i) {
                   if (!EqVec(values_lists[i], o.values_lists[i]))
                       return false;
               }
               return true;
           }() &&
           EqVec(sort_clause, o.sort_clause) && EqNode(limit_offset, o.limit_offset) &&
           EqNode(limit_count, o.limit_count) && limit_option == o.limit_option &&
           EqVec(locking_clause, o.locking_clause) && EqNode(with_clause, o.with_clause) &&
           op == o.op && all == o.all && EqNode(larg, o.larg) && EqNode(rarg, o.rarg);
}

Node* CreateStmt::Clone() const {
    auto* copy = makePallocNode<CreateStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->table_elts = CloneVec(table_elts);
    copy->inh_relations = CloneVec(inh_relations);
    copy->partbound = static_cast<PartitionBoundSpec*>(CloneNode(partbound));
    copy->partspec = static_cast<PartitionSpec*>(CloneNode(partspec));
    copy->of_typename = static_cast<TypeName*>(CloneNode(of_typename));
    copy->constraints = CloneVec(constraints);
    copy->options = CloneVec(options);
    return copy;
}

bool CreateStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(table_elts, o.table_elts) &&
           EqVec(inh_relations, o.inh_relations) && EqNode(partbound, o.partbound) &&
           EqNode(partspec, o.partspec) && EqNode(of_typename, o.of_typename) &&
           EqVec(constraints, o.constraints) && EqVec(options, o.options) &&
           oncommit == o.oncommit && tablespacename == o.tablespacename &&
           access_method == o.access_method && if_not_exists == o.if_not_exists;
}

Node* CreateSchemaStmt::Clone() const {
    auto* copy = makePallocNode<CreateSchemaStmt>(*this);
    copy->authrole = static_cast<RoleSpec*>(CloneNode(authrole));
    copy->schema_elts = CloneVec(schema_elts);
    return copy;
}

bool CreateSchemaStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateSchemaStmt&>(other);
    return schemaname == o.schemaname && EqNode(authrole, o.authrole) &&
           EqVec(schema_elts, o.schema_elts) && if_not_exists == o.if_not_exists;
}

Node* AlterTableStmt::Clone() const {
    auto* copy = makePallocNode<AlterTableStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->cmds = CloneVec(cmds);
    return copy;
}

bool AlterTableStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterTableStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(cmds, o.cmds) && objtype == o.objtype &&
           missing_ok == o.missing_ok;
}

Node* AlterTableCmd::Clone() const {
    auto* copy = makePallocNode<AlterTableCmd>(*this);
    copy->newowner = static_cast<RoleSpec*>(CloneNode(newowner));
    copy->def = CloneNode(def);
    return copy;
}

bool AlterTableCmd::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterTableCmd&>(other);
    return subtype == o.subtype && name == o.name && newname == o.newname && num == o.num &&
           EqNode(newowner, o.newowner) && EqNode(def, o.def) && behavior == o.behavior &&
           missing_ok == o.missing_ok && recurse == o.recurse;
}

Node* DropStmt::Clone() const {
    auto* copy = makePallocNode<DropStmt>(*this);
    copy->objects = CloneVec(objects);
    return copy;
}

bool DropStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DropStmt&>(other);
    return EqVec(objects, o.objects) && remove_type == o.remove_type && behavior == o.behavior &&
           missing_ok == o.missing_ok && concurrent == o.concurrent;
}

// ===========================================================================
// Additional statement node types (Phase 5 grammar expansion)
// ===========================================================================

Node* TransactionStmt::Clone() const {
    auto* copy = makePallocNode<TransactionStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool TransactionStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TransactionStmt&>(other);
    return kind == o.kind && savepoint_name == o.savepoint_name && EqVec(options, o.options) &&
           gid == o.gid;
}

Node* TruncateStmt::Clone() const {
    auto* copy = makePallocNode<TruncateStmt>(*this);
    copy->relations = CloneVec(relations);
    return copy;
}

bool TruncateStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const TruncateStmt&>(other);
    return EqVec(relations, o.relations) && restart_seqs == o.restart_seqs &&
           behavior == o.behavior;
}

Node* ExplainStmt::Clone() const {
    auto* copy = makePallocNode<ExplainStmt>(*this);
    copy->query = CloneNode(query);
    copy->options = CloneVec(options);
    return copy;
}

bool ExplainStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ExplainStmt&>(other);
    return EqNode(query, o.query) && EqVec(options, o.options);
}

Node* CommentStmt::Clone() const {
    auto* copy = makePallocNode<CommentStmt>(*this);
    copy->object = CloneVec(object);
    return copy;
}

bool CommentStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CommentStmt&>(other);
    return objtype == o.objtype && EqVec(object, o.object) && comment == o.comment;
}

Node* IndexStmt::Clone() const {
    auto* copy = makePallocNode<IndexStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->index_params = CloneVec(index_params);
    copy->index_including_params = CloneVec(index_including_params);
    copy->options = CloneVec(options);
    copy->where_clause = CloneVec(where_clause);
    return copy;
}

bool IndexStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const IndexStmt&>(other);
    return idxname == o.idxname && EqNode(relation, o.relation) &&
           access_method == o.access_method && EqVec(index_params, o.index_params) &&
           EqVec(index_including_params, o.index_including_params) && EqVec(options, o.options) &&
           EqVec(where_clause, o.where_clause) && unique == o.unique && primary == o.primary &&
           concurrent == o.concurrent && if_not_exists == o.if_not_exists;
}

Node* ViewStmt::Clone() const {
    auto* copy = makePallocNode<ViewStmt>(*this);
    copy->view = static_cast<RangeVar*>(CloneNode(view));
    copy->aliases = CloneVec(aliases);
    copy->query = CloneNode(query);
    copy->options = CloneVec(options);
    return copy;
}

bool ViewStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ViewStmt&>(other);
    return EqNode(view, o.view) && EqVec(aliases, o.aliases) && EqNode(query, o.query) &&
           replace == o.replace && EqVec(options, o.options) &&
           with_check_option == o.with_check_option;
}

Node* CreateAsStmt::Clone() const {
    auto* copy = makePallocNode<CreateAsStmt>(*this);
    copy->into = static_cast<IntoClause*>(CloneNode(into));
    copy->query = CloneNode(query);
    return copy;
}

bool CreateAsStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateAsStmt&>(other);
    return EqNode(into, o.into) && EqNode(query, o.query) && is_select_into == o.is_select_into &&
           if_not_exists == o.if_not_exists;
}

Node* VacuumStmt::Clone() const {
    auto* copy = makePallocNode<VacuumStmt>(*this);
    copy->options = CloneVec(options);
    copy->rels = CloneVec(rels);
    return copy;
}

bool VacuumStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const VacuumStmt&>(other);
    return EqVec(options, o.options) && EqVec(rels, o.rels) && is_vacuumcmd == o.is_vacuumcmd;
}

Node* VariableSetStmt::Clone() const {
    auto* copy = makePallocNode<VariableSetStmt>(*this);
    copy->args = CloneVec(args);
    return copy;
}

bool VariableSetStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const VariableSetStmt&>(other);
    return kind == o.kind && name == o.name && EqVec(args, o.args) && is_local == o.is_local;
}

Node* ClusterStmt::Clone() const {
    auto* copy = makePallocNode<ClusterStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    return copy;
}

bool ClusterStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ClusterStmt&>(other);
    return EqNode(relation, o.relation) && indexname == o.indexname && verbose == o.verbose;
}

Node* LockStmt::Clone() const {
    auto* copy = makePallocNode<LockStmt>(*this);
    copy->relations = CloneVec(relations);
    return copy;
}

bool LockStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const LockStmt&>(other);
    return EqVec(relations, o.relations) && mode == o.mode && nowait == o.nowait;
}

Node* DiscardStmt::Clone() const {
    auto* copy = makePallocNode<DiscardStmt>(*this);
    return copy;
}

bool DiscardStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DiscardStmt&>(other);
    return target == o.target;
}

Node* NotifyStmt::Clone() const {
    auto* copy = makePallocNode<NotifyStmt>(*this);
    return copy;
}

bool NotifyStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const NotifyStmt&>(other);
    return conditionname == o.conditionname && payload == o.payload;
}

Node* ListenStmt::Clone() const {
    auto* copy = makePallocNode<ListenStmt>(*this);
    return copy;
}

bool ListenStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ListenStmt&>(other);
    return conditionname == o.conditionname;
}

Node* UnlistenStmt::Clone() const {
    auto* copy = makePallocNode<UnlistenStmt>(*this);
    return copy;
}

bool UnlistenStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const UnlistenStmt&>(other);
    return conditionname == o.conditionname;
}

Node* CheckPointStmt::Clone() const {
    auto* copy = makePallocNode<CheckPointStmt>(*this);
    return copy;
}

bool CheckPointStmt::Equals(const Node& other) const {
    return other.GetTag() == GetTag();
}

Node* ReindexStmt::Clone() const {
    auto* copy = makePallocNode<ReindexStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool ReindexStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ReindexStmt&>(other);
    return kind == o.kind && name == o.name && EqVec(options, o.options) &&
           concurrently == o.concurrently;
}

Node* DeallocateStmt::Clone() const {
    auto* copy = makePallocNode<DeallocateStmt>(*this);
    return copy;
}

bool DeallocateStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DeallocateStmt&>(other);
    return name == o.name;
}

Node* PrepareStmt::Clone() const {
    auto* copy = makePallocNode<PrepareStmt>(*this);
    copy->argtypes = CloneVec(argtypes);
    copy->query = CloneNode(query);
    return copy;
}

bool PrepareStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const PrepareStmt&>(other);
    return name == o.name && EqVec(argtypes, o.argtypes) && EqNode(query, o.query);
}

Node* ExecuteStmt::Clone() const {
    auto* copy = makePallocNode<ExecuteStmt>(*this);
    copy->params = CloneVec(params);
    return copy;
}

bool ExecuteStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const ExecuteStmt&>(other);
    return name == o.name && EqVec(params, o.params);
}

Node* LoadStmt::Clone() const {
    auto* copy = makePallocNode<LoadStmt>(*this);
    return copy;
}

bool LoadStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const LoadStmt&>(other);
    return filename == o.filename;
}

Node* CallStmt::Clone() const {
    auto* copy = makePallocNode<CallStmt>(*this);
    copy->funccall = CloneNode(funccall);
    return copy;
}

bool CallStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CallStmt&>(other);
    return EqNode(funccall, o.funccall);
}

Node* RenameStmt::Clone() const {
    auto* copy = makePallocNode<RenameStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->object = CloneVec(object);
    return copy;
}

bool RenameStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RenameStmt&>(other);
    return rename_type == o.rename_type && relation_type == o.relation_type &&
           EqNode(relation, o.relation) && EqVec(object, o.object) && subname == o.subname &&
           newname == o.newname && behavior == o.behavior && missing_ok == o.missing_ok;
}

Node* AlterOwnerStmt::Clone() const {
    auto* copy = makePallocNode<AlterOwnerStmt>(*this);
    copy->object = CloneVec(object);
    copy->newowner = static_cast<RoleSpec*>(CloneNode(newowner));
    return copy;
}

bool AlterOwnerStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterOwnerStmt&>(other);
    return object_type == o.object_type && EqVec(object, o.object) && EqNode(newowner, o.newowner);
}

Node* CreateSeqStmt::Clone() const {
    auto* copy = makePallocNode<CreateSeqStmt>(*this);
    copy->sequence = static_cast<RangeVar*>(CloneNode(sequence));
    copy->options = CloneVec(options);
    return copy;
}

bool CreateSeqStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateSeqStmt&>(other);
    return EqNode(sequence, o.sequence) && EqVec(options, o.options) &&
           for_identity == o.for_identity && if_not_exists == o.if_not_exists;
}

Node* AlterSeqStmt::Clone() const {
    auto* copy = makePallocNode<AlterSeqStmt>(*this);
    copy->sequence = static_cast<RangeVar*>(CloneNode(sequence));
    copy->options = CloneVec(options);
    return copy;
}

bool AlterSeqStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterSeqStmt&>(other);
    return EqNode(sequence, o.sequence) && EqVec(options, o.options) &&
           for_identity == o.for_identity && missing_ok == o.missing_ok;
}

Node* CreateFunctionStmt::Clone() const {
    auto* copy = makePallocNode<CreateFunctionStmt>(*this);
    copy->funcname = CloneVec(funcname);
    copy->parameters = CloneVec(parameters);
    copy->return_type = static_cast<TypeName*>(CloneNode(return_type));
    copy->options = CloneVec(options);
    return copy;
}

bool CreateFunctionStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateFunctionStmt&>(other);
    return is_procedure == o.is_procedure && replace == o.replace && EqVec(funcname, o.funcname) &&
           EqVec(parameters, o.parameters) && EqNode(return_type, o.return_type) &&
           EqVec(options, o.options);
}

Node* AlterFunctionStmt::Clone() const {
    auto* copy = makePallocNode<AlterFunctionStmt>(*this);
    copy->funcname = CloneVec(funcname);
    copy->args = CloneVec(args);
    copy->actions = CloneVec(actions);
    return copy;
}

bool AlterFunctionStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterFunctionStmt&>(other);
    return EqVec(funcname, o.funcname) && EqVec(args, o.args) && EqVec(actions, o.actions);
}

Node* CreateTrigStmt::Clone() const {
    auto* copy = makePallocNode<CreateTrigStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->funcname = CloneVec(funcname);
    copy->args = CloneVec(args);
    copy->columns = CloneVec(columns);
    copy->when_clause = CloneNode(when_clause);
    copy->transition_rels = CloneVec(transition_rels);
    return copy;
}

bool CreateTrigStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateTrigStmt&>(other);
    return replace == o.replace && isconstraint == o.isconstraint && trigname == o.trigname &&
           EqNode(relation, o.relation) && EqVec(funcname, o.funcname) && EqVec(args, o.args) &&
           row == o.row && timing == o.timing && events == o.events && EqVec(columns, o.columns) &&
           EqNode(when_clause, o.when_clause) && EqVec(transition_rels, o.transition_rels) &&
           deferrable == o.deferrable && initdeferred == o.initdeferred && constrrel == o.constrrel;
}

Node* CreateRoleStmt::Clone() const {
    auto* copy = makePallocNode<CreateRoleStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool CreateRoleStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateRoleStmt&>(other);
    return stmt_type == o.stmt_type && role == o.role && EqVec(options, o.options);
}

Node* AlterRoleStmt::Clone() const {
    auto* copy = makePallocNode<AlterRoleStmt>(*this);
    copy->role = static_cast<RoleSpec*>(CloneNode(role));
    copy->options = CloneVec(options);
    return copy;
}

bool AlterRoleStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterRoleStmt&>(other);
    return EqNode(role, o.role) && EqVec(options, o.options) && action == o.action;
}

Node* DropRoleStmt::Clone() const {
    auto* copy = makePallocNode<DropRoleStmt>(*this);
    copy->roles = CloneVec(roles);
    return copy;
}

bool DropRoleStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DropRoleStmt&>(other);
    return EqVec(roles, o.roles) && missing_ok == o.missing_ok;
}

Node* GrantStmt::Clone() const {
    auto* copy = makePallocNode<GrantStmt>(*this);
    copy->privileges = CloneVec(privileges);
    copy->targobjs = CloneVec(targobjs);
    copy->grantees = CloneVec(grantees);
    return copy;
}

bool GrantStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const GrantStmt&>(other);
    return is_grant == o.is_grant && targtype == o.targtype && objtype == o.objtype &&
           EqVec(privileges, o.privileges) && EqVec(targobjs, o.targobjs) &&
           grant_option == o.grant_option && EqVec(grantees, o.grantees) && behavior == o.behavior;
}

Node* GrantRoleStmt::Clone() const {
    auto* copy = makePallocNode<GrantRoleStmt>(*this);
    copy->granted_roles = CloneVec(granted_roles);
    copy->grantee_roles = CloneVec(grantee_roles);
    copy->grantor = static_cast<RoleSpec*>(CloneNode(grantor));
    return copy;
}

bool GrantRoleStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const GrantRoleStmt&>(other);
    return EqVec(granted_roles, o.granted_roles) && EqVec(grantee_roles, o.grantee_roles) &&
           is_grant == o.is_grant && admin_opt == o.admin_opt && EqNode(grantor, o.grantor) &&
           behavior == o.behavior;
}

Node* CopyStmt::Clone() const {
    auto* copy = makePallocNode<CopyStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    copy->attlist = CloneVec(attlist);
    copy->options = CloneVec(options);
    copy->query = CloneNode(query);
    return copy;
}

bool CopyStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CopyStmt&>(other);
    return EqNode(relation, o.relation) && EqVec(attlist, o.attlist) && is_from == o.is_from &&
           is_program == o.is_program && filename == o.filename && EqVec(options, o.options) &&
           EqNode(query, o.query);
}

Node* RefreshMatViewStmt::Clone() const {
    auto* copy = makePallocNode<RefreshMatViewStmt>(*this);
    copy->relation = static_cast<RangeVar*>(CloneNode(relation));
    return copy;
}

bool RefreshMatViewStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const RefreshMatViewStmt&>(other);
    return concurrent == o.concurrent && skip_data == o.skip_data && EqNode(relation, o.relation);
}

Node* CreateTableSpaceStmt::Clone() const {
    auto* copy = makePallocNode<CreateTableSpaceStmt>(*this);
    copy->owner = static_cast<RoleSpec*>(CloneNode(owner));
    copy->options = CloneVec(options);
    return copy;
}

bool CreateTableSpaceStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreateTableSpaceStmt&>(other);
    return tablespacename == o.tablespacename && EqNode(owner, o.owner) && location == o.location &&
           EqVec(options, o.options);
}

Node* DropTableSpaceStmt::Clone() const {
    auto* copy = makePallocNode<DropTableSpaceStmt>(*this);
    return copy;
}

bool DropTableSpaceStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DropTableSpaceStmt&>(other);
    return tablespacename == o.tablespacename && missing_ok == o.missing_ok;
}

Node* CreatedbStmt::Clone() const {
    auto* copy = makePallocNode<CreatedbStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool CreatedbStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const CreatedbStmt&>(other);
    return dbname == o.dbname && EqVec(options, o.options);
}

Node* DropdbStmt::Clone() const {
    auto* copy = makePallocNode<DropdbStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool DropdbStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const DropdbStmt&>(other);
    return dbname == o.dbname && missing_ok == o.missing_ok && EqVec(options, o.options);
}

Node* AlterDatabaseStmt::Clone() const {
    auto* copy = makePallocNode<AlterDatabaseStmt>(*this);
    copy->options = CloneVec(options);
    return copy;
}

bool AlterDatabaseStmt::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const AlterDatabaseStmt&>(other);
    return dbname == o.dbname && EqVec(options, o.options);
}

// ===========================================================================
// Query tree
// ===========================================================================

Node* Query::Clone() const {
    auto* copy = makePallocNode<Query>(*this);
    copy->utility_stmt = CloneNode(utility_stmt);
    copy->cte_list = CloneVec(cte_list);
    copy->rtable = CloneVec(rtable);
    copy->jointree = CloneNode(jointree);
    copy->merge_action_list = CloneVec(merge_action_list);
    copy->target_list = CloneVec(target_list);
    copy->on_conflict = CloneNode(on_conflict);
    copy->returning_list = CloneVec(returning_list);
    copy->group_clause = CloneVec(group_clause);
    copy->grouping_sets = CloneVec(grouping_sets);
    copy->having_qual = CloneNode(having_qual);
    copy->window_clause = CloneVec(window_clause);
    copy->distinct_clause = CloneVec(distinct_clause);
    copy->sort_clause = CloneVec(sort_clause);
    copy->limit_offset = CloneNode(limit_offset);
    copy->limit_count = CloneNode(limit_count);
    copy->row_marks = CloneVec(row_marks);
    copy->set_operations = CloneNode(set_operations);
    copy->constraint_deps = CloneVec(constraint_deps);
    copy->with_check_options = CloneVec(with_check_options);
    return copy;
}

bool Query::Equals(const Node& other) const {
    if (other.GetTag() != GetTag())
        return false;
    const auto& o = static_cast<const Query&>(other);
    return command_type == o.command_type && query_source == o.query_source &&
           query_id == o.query_id && can_set_tag == o.can_set_tag &&
           EqNode(utility_stmt, o.utility_stmt) && result_relation == o.result_relation &&
           has_aggs == o.has_aggs && has_window_funcs == o.has_window_funcs &&
           has_target_srfs == o.has_target_srfs && has_sub_links == o.has_sub_links &&
           has_distinct_on == o.has_distinct_on && has_recursive == o.has_recursive &&
           has_modifying_cte == o.has_modifying_cte && has_for_update == o.has_for_update &&
           has_row_security == o.has_row_security && is_return == o.is_return &&
           EqVec(cte_list, o.cte_list) && EqVec(rtable, o.rtable) && EqNode(jointree, o.jointree) &&
           EqVec(merge_action_list, o.merge_action_list) &&
           merge_use_outer_join == o.merge_use_outer_join && EqVec(target_list, o.target_list) &&
           override_kind == o.override_kind && EqNode(on_conflict, o.on_conflict) &&
           EqVec(returning_list, o.returning_list) && EqVec(group_clause, o.group_clause) &&
           group_distinct == o.group_distinct && EqVec(grouping_sets, o.grouping_sets) &&
           EqNode(having_qual, o.having_qual) && EqVec(window_clause, o.window_clause) &&
           EqVec(distinct_clause, o.distinct_clause) && EqVec(sort_clause, o.sort_clause) &&
           EqNode(limit_offset, o.limit_offset) && EqNode(limit_count, o.limit_count) &&
           limit_option == o.limit_option && EqVec(row_marks, o.row_marks) &&
           EqNode(set_operations, o.set_operations) && EqVec(constraint_deps, o.constraint_deps) &&
           EqVec(with_check_options, o.with_check_options) && stmt_location == o.stmt_location &&
           stmt_len == o.stmt_len;
}

}  // namespace pgcpp::parser
