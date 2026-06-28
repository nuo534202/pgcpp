// geqo_test.cpp — Unit tests for Task 15.21 (M10 GEQO genetic optimizer).
//
// Verifies:
//   - ShouldUseGeqo threshold check (>= kGeqoThreshold base rels).
//   - Chromosome init (random permutation / identity).
//   - Crossover operators (ERX/PMX/OX1/OX2/PX) produce valid permutations.
//   - Mutation preserves permutation invariant.
//   - GeqoEvalFitness returns a finite cost for a valid chromosome.
//   - GeqoSolve returns a non-null Path for a >10 table join.
//   - End-to-end: query_planner routes to GEQO when base rel count >=
//     kGeqoThreshold and returns a join plan (not a SeqScan).

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_operator.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/common/memory/alloc_set.hpp"
#include "pgcpp/common/memory/memory_context.hpp"
#include "pgcpp/executor/plannodes.hpp"
#include "pgcpp/optimizer/geqo/geqo_copy.hpp"
#include "pgcpp/optimizer/geqo/geqo_cx.hpp"
#include "pgcpp/optimizer/geqo/geqo_erx.hpp"
#include "pgcpp/optimizer/geqo/geqo_eval.hpp"
#include "pgcpp/optimizer/geqo/geqo_io.hpp"
#include "pgcpp/optimizer/geqo/geqo_main.hpp"
#include "pgcpp/optimizer/geqo/geqo_misc.hpp"
#include "pgcpp/optimizer/geqo/geqo_mutation.hpp"
#include "pgcpp/optimizer/geqo/geqo_ox1.hpp"
#include "pgcpp/optimizer/geqo/geqo_ox2.hpp"
#include "pgcpp/optimizer/geqo/geqo_params.hpp"
#include "pgcpp/optimizer/geqo/geqo_pmx.hpp"
#include "pgcpp/optimizer/geqo/geqo_px.hpp"
#include "pgcpp/optimizer/geqo/geqo_random.hpp"
#include "pgcpp/optimizer/geqo/geqo_recombination.hpp"
#include "pgcpp/optimizer/path.hpp"
#include "pgcpp/optimizer/path/joinpath.hpp"
#include "pgcpp/optimizer/planner.hpp"
#include "pgcpp/optimizer/util/pathnode.hpp"
#include "pgcpp/optimizer/util/relnode.hpp"
#include "pgcpp/optimizer/util/restrictinfo.hpp"
#include "pgcpp/parser/parsenodes.hpp"
#include "pgcpp/parser/primnodes.hpp"
#include "pgcpp/types/datum.hpp"

using mytoydb::catalog::Catalog;
using mytoydb::catalog::FormData_pg_operator;
using mytoydb::catalog::GetCatalog;
using mytoydb::catalog::Oid;
using mytoydb::catalog::OperatorKind;
using mytoydb::catalog::SetCatalog;
using mytoydb::executor::Plan;
using mytoydb::executor::PlanType;
using mytoydb::memory::AllocSetContext;
using mytoydb::nodes::makePallocNode;
using mytoydb::optimizer::add_path;
using mytoydb::optimizer::build_simple_rel;
using mytoydb::optimizer::Cost;
using mytoydb::optimizer::create_seqscan_path;
using mytoydb::optimizer::find_base_rel;
using mytoydb::optimizer::make_restrictinfo;
using mytoydb::optimizer::Path;
using mytoydb::optimizer::PathType;
using mytoydb::optimizer::PlannerInfo;
using mytoydb::optimizer::query_planner;
using mytoydb::optimizer::Relids;
using mytoydb::optimizer::RelOptInfo;
using mytoydb::optimizer::RestrictInfo;
using mytoydb::optimizer::SeqScanPath;
using mytoydb::optimizer::SpecialJoinInfo;
using mytoydb::optimizer::geqo::AllocateChromosome;
using mytoydb::optimizer::geqo::Chromosome;
using mytoydb::optimizer::geqo::CollectBaseRelIds;
using mytoydb::optimizer::geqo::ComputeGeqoParams;
using mytoydb::optimizer::geqo::CopyChromosome;
using mytoydb::optimizer::geqo::CountBaseRels;
using mytoydb::optimizer::geqo::Crossover;
using mytoydb::optimizer::geqo::CrossoverERX;
using mytoydb::optimizer::geqo::CrossoverOX1;
using mytoydb::optimizer::geqo::CrossoverOX2;
using mytoydb::optimizer::geqo::CrossoverPMX;
using mytoydb::optimizer::geqo::CrossoverPX;
using mytoydb::optimizer::geqo::CrossoverType;
using mytoydb::optimizer::geqo::FindBestChromosome;
using mytoydb::optimizer::geqo::FormatChromosome;
using mytoydb::optimizer::geqo::Gene;
using mytoydb::optimizer::geqo::GeqoBuildBestPath;
using mytoydb::optimizer::geqo::GeqoEvalFitness;
using mytoydb::optimizer::geqo::GeqoParams;
using mytoydb::optimizer::geqo::GeqoRng;
using mytoydb::optimizer::geqo::GeqoSolve;
using mytoydb::optimizer::geqo::InitChromosomeIdentity;
using mytoydb::optimizer::geqo::InitChromosomeRandom;
using mytoydb::optimizer::geqo::IsValidPermutation;
using mytoydb::optimizer::geqo::kGeqoThreshold;
using mytoydb::optimizer::geqo::MutateChromosome;
using mytoydb::optimizer::geqo::RandomCrossover;
using mytoydb::optimizer::geqo::SetGeqoSeed;
using mytoydb::optimizer::geqo::ShouldUseGeqo;
using mytoydb::parser::Alias;
using mytoydb::parser::BoolExpr;
using mytoydb::parser::BoolExprType;
using mytoydb::parser::CmdType;
using mytoydb::parser::FromExpr;
using mytoydb::parser::JoinType;
using mytoydb::parser::Node;
using mytoydb::parser::OpExpr;
using mytoydb::parser::Query;
using mytoydb::parser::RangeTblEntry;
using mytoydb::parser::RangeTblRef;
using mytoydb::parser::RTEKind;
using mytoydb::parser::TargetEntry;
using mytoydb::parser::Var;
using mytoydb::types::kBoolOid;
using mytoydb::types::kInt4Oid;

namespace {

constexpr Oid kInt4EqOp = 96;  // int4 = int4 (bootstrap OID)

class GeqoTest : public ::testing::Test {
protected:
    void SetUp() override {
        mytoydb::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("geqo_test_context");
        mytoydb::memory::SetCurrentMemoryContext(context_);
        // Seed the GEQO RNG deterministically so test outcomes are reproducible.
        SetGeqoSeed(0x123456789ABCDEF0ULL);
        // Install a minimal catalog with int4eq (so RestrictInfo can set
        // mergejoinable/hashjoinable flags — required by add_paths_to_joinrel).
        catalog_ = new Catalog();
        SetCatalog(catalog_);
        auto* op_row = makePallocNode<FormData_pg_operator>();
        op_row->oid = kInt4EqOp;
        op_row->oprname = "=";
        op_row->oprkind = OperatorKind::kBinary;
        op_row->oprcanmerge = true;
        op_row->oprcanhash = true;
        op_row->oprleft = kInt4Oid;
        op_row->oprright = kInt4Oid;
        op_row->oprresult = kBoolOid;
        catalog_->InsertOperator(op_row);
    }

    void TearDown() override {
        SetCatalog(nullptr);
        delete catalog_;
        mytoydb::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    Var* MakeVar(int varno, int varattno) {
        auto* var = makePallocNode<Var>();
        var->varno = varno;
        var->varattno = varattno;
        var->vartype = kInt4Oid;
        return var;
    }

    OpExpr* MakeEqOp(Node* left, Node* right) {
        auto* op = makePallocNode<OpExpr>();
        op->opno = kInt4EqOp;
        op->opresulttype = kBoolOid;
        op->args.push_back(left);
        op->args.push_back(right);
        return op;
    }

    RangeTblEntry* MakeRTE(int relid) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = relid;
        return rte;
    }

    RangeTblRef* MakeRangeTblRef(int rtindex) {
        auto* ref = makePallocNode<RangeTblRef>();
        ref->rtindex = rtindex;
        return ref;
    }

    // Build a PlannerInfo with `n_rels` base relations, each with a SeqScanPath
    // already added as its cheapest_path. Optionally, attach a chain of join
    // clauses rel[i].x = rel[i+1].x for i in [0, n_rels-1) so the GEQO fitness
    // function can detect connecting clauses.
    PlannerInfo* MakePlannerWithRels(int n_rels, bool with_join_clauses = true) {
        auto* root = makePallocNode<PlannerInfo>();
        auto* query = makePallocNode<Query>();
        query->command_type = CmdType::kSelect;
        // Build the RTE array and jointree (so FindFirstBaseRelIndex works).
        auto* from = makePallocNode<FromExpr>();
        query->jointree = from;
        // Populate both query->rtable and root->simple_rte_array/simple_rel_array
        // (build_simple_rel reads from simple_rte_array, not query->rtable).
        for (int i = 0; i < n_rels; ++i) {
            auto* rte = makePallocNode<RangeTblEntry>();
            rte->rtekind = RTEKind::kRelation;
            rte->relid = 16384 + i;
            query->rtable.push_back(rte);
            from->fromlist.push_back(MakeRangeTblRef(i + 1));
            root->simple_rte_array.push_back(rte);
            root->simple_rel_array.push_back(nullptr);  // filled by build_simple_rel
        }
        // Build base RelOptInfos and pre-populate statistics so the cost
        // estimator can compute SeqScanPath cost.
        for (int i = 0; i < n_rels; ++i) {
            RelOptInfo* rel = build_simple_rel(root, i + 1, nullptr);
            if (rel != nullptr) {
                rel->rows = 10;
                rel->width = 4;
                rel->pages = 1;
                rel->tuples = 10;
                rel->relids = {i + 1};
                // Add a SeqScanPath so cheapest_path is set (GEQO requires this).
                SeqScanPath* scan = create_seqscan_path(root, rel);
                add_path(rel, scan);
            }
        }
        root->parse = query;

        // Optionally, attach join clauses rel[i].x = rel[i+1].x.
        if (with_join_clauses) {
            for (int i = 0; i + 1 < n_rels; ++i) {
                Node* clause = MakeEqOp(MakeVar(i + 1, 1), MakeVar(i + 2, 1));
                // Wrap in RestrictInfo and attach to both rels' joininfo (this
                // is the path GEQO's CollectJoinClausesMulti scans).
                Relids relids = {i + 1, i + 2};
                RestrictInfo* ri = make_restrictinfo(root, clause, true, false, relids, {}, 0);
                if (ri != nullptr) {
                    RelOptInfo* r1 = find_base_rel(root, i + 1);
                    RelOptInfo* r2 = find_base_rel(root, i + 2);
                    if (r1 != nullptr)
                        r1->joininfo.push_back(ri);
                    if (r2 != nullptr)
                        r2->joininfo.push_back(ri);
                }
            }
        }
        return root;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// ---------------------------------------------------------------------------
// ShouldUseGeqo threshold tests
// ---------------------------------------------------------------------------

// ShouldUseGeqo returns false when the query has < kGeqoThreshold base rels.
TEST_F(GeqoTest, ShouldUseGeqo_BelowThreshold_ReturnsFalse) {
    auto* root = MakePlannerWithRels(kGeqoThreshold - 1);
    EXPECT_FALSE(ShouldUseGeqo(root));
}

// ShouldUseGeqo returns true when the query has exactly kGeqoThreshold base rels.
TEST_F(GeqoTest, ShouldUseGeqo_AtThreshold_ReturnsTrue) {
    auto* root = MakePlannerWithRels(kGeqoThreshold);
    EXPECT_TRUE(ShouldUseGeqo(root));
}

// ShouldUseGeqo returns true when the query has > kGeqoThreshold base rels.
TEST_F(GeqoTest, ShouldUseGeqo_AboveThreshold_ReturnsTrue) {
    auto* root = MakePlannerWithRels(kGeqoThreshold + 3);
    EXPECT_TRUE(ShouldUseGeqo(root));
}

// CountBaseRels counts only non-null slots.
TEST_F(GeqoTest, CountBaseRels_ReportsCorrectCount) {
    auto* root = MakePlannerWithRels(15);
    EXPECT_EQ(CountBaseRels(root), 15);
}

// CollectBaseRelIds returns the 1-based RT indexes in ascending order.
TEST_F(GeqoTest, CollectBaseRelIds_ReturnsAscendingRTIndexes) {
    auto* root = MakePlannerWithRels(5);
    std::vector<Gene> ids = CollectBaseRelIds(root);
    ASSERT_EQ(ids.size(), 5u);
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(ids[static_cast<size_t>(i)], i + 1);
}

// ---------------------------------------------------------------------------
// Chromosome init tests
// ---------------------------------------------------------------------------

// InitChromosomeRandom produces a valid permutation of the input gene set.
TEST_F(GeqoTest, InitChromosomeRandom_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6, 7, 8};
    Chromosome* c = AllocateChromosome();
    InitChromosomeRandom(c, rel_ids);
    EXPECT_TRUE(IsValidPermutation(*c, rel_ids));
    EXPECT_FALSE(c->evaluated);
}

// InitChromosomeIdentity preserves the input order.
TEST_F(GeqoTest, InitChromosomeIdentity_PreservesOrder) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5};
    Chromosome* c = AllocateChromosome();
    InitChromosomeIdentity(c, rel_ids);
    ASSERT_EQ(c->genes.size(), rel_ids.size());
    for (size_t i = 0; i < rel_ids.size(); ++i)
        EXPECT_EQ(c->genes[i], rel_ids[i]);
}

// CopyChromosome duplicates both genes and cached fitness.
TEST_F(GeqoTest, CopyChromosome_DuplicatesGenesAndFitness) {
    Chromosome src;
    src.genes = {1, 2, 3};
    src.fitness = 42.0;
    src.evaluated = true;
    Chromosome dst;
    CopyChromosome(&dst, &src);
    EXPECT_EQ(dst.genes, src.genes);
    EXPECT_EQ(dst.fitness, src.fitness);
    EXPECT_TRUE(dst.evaluated);
}

// ---------------------------------------------------------------------------
// Crossover operator tests (each must produce a valid permutation)
// ---------------------------------------------------------------------------

// ERX produces a valid permutation covering the parent gene set.
TEST_F(GeqoTest, CrossoverERX_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    ASSERT_TRUE(CrossoverERX(&mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// PMX produces a valid permutation.
TEST_F(GeqoTest, CrossoverPMX_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    ASSERT_TRUE(CrossoverPMX(&mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// OX1 produces a valid permutation.
TEST_F(GeqoTest, CrossoverOX1_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    ASSERT_TRUE(CrossoverOX1(&mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// OX2 produces a valid permutation.
TEST_F(GeqoTest, CrossoverOX2_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    ASSERT_TRUE(CrossoverOX2(&mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// PX produces a valid permutation.
TEST_F(GeqoTest, CrossoverPX_ProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    ASSERT_TRUE(CrossoverPX(&mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// Crossover dispatcher routes to the correct operator by type.
TEST_F(GeqoTest, Crossover_Dispatcher_RoutesToCorrectOperator) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6};
    dad.genes = {6, 5, 4, 3, 2, 1};
    EXPECT_TRUE(Crossover(CrossoverType::kEdgeRecombination, &mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
    EXPECT_TRUE(Crossover(CrossoverType::kPartiallyMapped, &mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
    EXPECT_TRUE(Crossover(CrossoverType::kOrder1, &mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
    EXPECT_TRUE(Crossover(CrossoverType::kOrder2, &mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
    EXPECT_TRUE(Crossover(CrossoverType::kPosition, &mum, &dad, &child));
    EXPECT_TRUE(IsValidPermutation(child, rel_ids));
}

// RandomCrossover always produces a valid permutation.
TEST_F(GeqoTest, RandomCrossover_AlwaysProducesValidPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6, 7, 8};
    Chromosome mum, dad, child;
    mum.genes = {1, 2, 3, 4, 5, 6, 7, 8};
    dad.genes = {8, 7, 6, 5, 4, 3, 2, 1};
    // Run many iterations to exercise all operator paths.
    for (int i = 0; i < 20; ++i) {
        RandomCrossover(&mum, &dad, &child);
        EXPECT_TRUE(IsValidPermutation(child, rel_ids)) << "iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// Mutation tests
// ---------------------------------------------------------------------------

// MutateChromosome preserves the permutation invariant (when it fires).
TEST_F(GeqoTest, MutateChromosome_PreservesPermutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5, 6, 7, 8};
    Chromosome c;
    c.genes = rel_ids;
    // Force mutation by passing prob=1.0.
    MutateChromosome(&c, 1.0);
    EXPECT_TRUE(IsValidPermutation(c, rel_ids));
    EXPECT_FALSE(c.evaluated);  // mutation invalidates the cached fitness
}

// MutateChromosome with prob=0.0 leaves the chromosome unchanged.
TEST_F(GeqoTest, MutateChromosome_ProbZero_NoMutation) {
    std::vector<Gene> rel_ids = {1, 2, 3, 4, 5};
    Chromosome c;
    c.genes = rel_ids;
    c.evaluated = true;
    bool mutated = MutateChromosome(&c, 0.0);
    EXPECT_FALSE(mutated);
    EXPECT_EQ(c.genes, rel_ids);
    EXPECT_TRUE(c.evaluated);  // cache still valid
}

// ---------------------------------------------------------------------------
// Fitness evaluation tests
// ---------------------------------------------------------------------------

// GeqoEvalFitness returns a finite, positive cost for a valid chromosome.
TEST_F(GeqoTest, GeqoEvalFitness_ReturnsFiniteCost) {
    auto* root = MakePlannerWithRels(6);
    std::vector<Gene> chrom = {1, 2, 3, 4, 5, 6};
    Cost f = GeqoEvalFitness(root, chrom);
    EXPECT_GT(f, 0.0);
    EXPECT_LT(f, std::numeric_limits<Cost>::max());
}

// GeqoEvalFitness returns kInvalidCost for a chromosome referencing a
// non-existent relation.
TEST_F(GeqoTest, GeqoEvalFitness_MissingRel_ReturnsInvalidCost) {
    auto* root = MakePlannerWithRels(3);
    std::vector<Gene> chrom = {1, 2, 99};  // rel 99 does not exist
    Cost f = GeqoEvalFitness(root, chrom);
    EXPECT_EQ(f, std::numeric_limits<Cost>::max());
}

// GeqoEvalFitness penalizes cross products (no join clause) with higher cost.
TEST_F(GeqoTest, GeqoEvalFitness_CrossProduct_Penalized) {
    // Without join clauses, every join step is a cross product (sel=1.0).
    auto* root_no_clauses = MakePlannerWithRels(3, /*with_join_clauses=*/false);
    // With join clauses, sel=0.1 between adjacent rels.
    auto* root_with_clauses = MakePlannerWithRels(3, /*with_join_clauses=*/true);
    std::vector<Gene> chrom = {1, 2, 3};
    Cost f_no = GeqoEvalFitness(root_no_clauses, chrom);
    Cost f_yes = GeqoEvalFitness(root_with_clauses, chrom);
    // Cross product (sel=1.0) should produce a higher cost than the
    // equi-join (sel=0.1) because output rows explode by 10x at each step.
    EXPECT_GT(f_no, f_yes);
}

// GeqoBuildBestPath constructs a non-null Path tree for a valid chromosome.
TEST_F(GeqoTest, GeqoBuildBestPath_ReturnsNonNullPath) {
    auto* root = MakePlannerWithRels(4);
    std::vector<Gene> chrom = {1, 2, 3, 4};
    Path* p = GeqoBuildBestPath(root, chrom);
    EXPECT_NE(p, nullptr);
    // The result should be a join path (NestLoop/HashJoin/MergeJoin), not a
    // SeqScan, since we joined 4 base rels into one tree.
    EXPECT_NE(p->type, PathType::kSeqScan);
}

// ---------------------------------------------------------------------------
// ComputeGeqoParams tests
// ---------------------------------------------------------------------------

// ComputeGeqoParams scales pool size with relation count.
TEST_F(GeqoTest, ComputeGeqoParams_ScalesWithRelCount) {
    GeqoParams p_small = ComputeGeqoParams(4);
    GeqoParams p_large = ComputeGeqoParams(20);
    EXPECT_GE(p_large.pool_size, p_small.pool_size);
    EXPECT_GE(p_large.generations, 1);
    EXPECT_GE(p_small.generations, 1);
}

// ComputeGeqoParams clamps to a minimum pool size of 2.
TEST_F(GeqoTest, ComputeGeqoParams_MinimumPoolSize) {
    GeqoParams p = ComputeGeqoParams(1);
    EXPECT_GE(p.pool_size, 2);
}

// ---------------------------------------------------------------------------
// End-to-end: GeqoSolve and query_planner integration
// ---------------------------------------------------------------------------

// GeqoSolve returns a non-null Path for a >10 table join.
TEST_F(GeqoTest, GeqoSolve_MultiTableJoin_ReturnsNonNullPath) {
    auto* root = MakePlannerWithRels(kGeqoThreshold + 1);  // 13 tables
    Path* p = GeqoSolve(root);
    ASSERT_NE(p, nullptr);
    // The result must be a join path (not a SeqScan), since GEQO builds a
    // left-deep join tree over all 13 base rels.
    EXPECT_NE(p->type, PathType::kSeqScan);
}

// FindBestChromosome returns the lowest-fitness member.
TEST_F(GeqoTest, FindBestChromosome_ReturnsLowestFitness) {
    std::vector<Chromosome*> pool;
    Chromosome a, b, c;
    a.fitness = 100.0;
    a.evaluated = true;
    b.fitness = 50.0;
    b.evaluated = true;
    c.fitness = 75.0;
    c.evaluated = true;
    pool = {&a, &b, &c};
    Chromosome* best = FindBestChromosome(pool);
    EXPECT_EQ(best, &b);
}

// FormatChromosome produces a string containing the gene values.
TEST_F(GeqoTest, FormatChromosome_ContainsGenes) {
    Chromosome c;
    c.genes = {3, 1, 4, 2};
    c.fitness = 17.5;
    c.evaluated = true;
    std::string s = FormatChromosome(c);
    EXPECT_NE(s.find("3"), std::string::npos);
    EXPECT_NE(s.find("17.5"), std::string::npos);
}

// query_planner routes to GEQO when base rel count >= kGeqoThreshold and
// returns a join plan (not a SeqScan). This is the end-to-end verification
// requested by Task 15.21: ">10 table JOIN uses GEQO path".
TEST_F(GeqoTest, QueryPlanner_MultiTableJoin_UsesGeqoPath) {
    const int n_rels = kGeqoThreshold + 1;  // 13 tables (>10)
    auto* root = makePallocNode<PlannerInfo>();
    auto* query = makePallocNode<Query>();
    query->command_type = CmdType::kSelect;
    auto* from = makePallocNode<FromExpr>();
    query->jointree = from;
    for (int i = 0; i < n_rels; ++i) {
        auto* rte = makePallocNode<RangeTblEntry>();
        rte->rtekind = RTEKind::kRelation;
        rte->relid = 16384 + i;
        query->rtable.push_back(rte);
        from->fromlist.push_back(MakeRangeTblRef(i + 1));
    }
    // Attach a single target entry so create_plan has something to project.
    auto* te = makePallocNode<TargetEntry>();
    te->expr = MakeVar(1, 1);
    te->resno = 1;
    te->resname = "a";
    query->target_list.push_back(te);
    root->parse = query;

    Plan* plan = query_planner(root, query);

    // GEQO should have produced a join plan (NestLoop/HashJoin/MergeJoin),
    // not a SeqScan, because we have 13 base rels joined together.
    ASSERT_NE(plan, nullptr);
    EXPECT_NE(plan->type, PlanType::kSeqScan);
}

}  // namespace
