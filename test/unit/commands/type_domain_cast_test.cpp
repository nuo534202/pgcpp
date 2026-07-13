// type_domain_cast_test.cpp — Unit tests for CREATE TYPE/DOMAIN/CAST (P2-10).
//
// Tests the full parse → ProcessUtility → command handler path for:
//   CREATE TYPE ... AS ENUM
//   CREATE DOMAIN ... AS type
//   CREATE CAST (source AS target) WITH/WITHOUT FUNCTION

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "access/rel.hpp"
#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_cast.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_type.hpp"
#include "catalog/syscache.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/analyze.hpp"
#include "parser/parsenodes.hpp"
#include "parser/parser.hpp"
#include "protocol/pqformat.hpp"
#include "protocol/utility.hpp"
#include "storage/bufmgr.hpp"
#include "storage/smgr.hpp"
#include "transaction/snapshot.hpp"
#include "transaction/transam.hpp"
#include "transaction/xact.hpp"
#include "types/datum.hpp"

using pgcpp::access::InitializeRelcache;
using pgcpp::access::ResetRelcache;
using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::CastContext;
using pgcpp::catalog::CastMethod;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_cast;
using pgcpp::catalog::FormData_pg_type;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::SetCatalog;
using pgcpp::catalog::SetSysCache;
using pgcpp::catalog::SysCache;
using pgcpp::catalog::TypeType;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Node;
using pgcpp::parser::parse_analyze;
using pgcpp::parser::Query;
using pgcpp::parser::raw_parser;
using pgcpp::parser::RawStmt;
using pgcpp::protocol::ProcessUtility;
using pgcpp::protocol::StringSink;
using pgcpp::storage::InitBufferPool;
using pgcpp::storage::SetStorageBaseDir;
using pgcpp::storage::ShutdownBufferPool;
using pgcpp::storage::smgrcloseall;
using pgcpp::transaction::BeginTransactionBlock;
using pgcpp::transaction::EndTransactionBlock;
using pgcpp::transaction::InitializeSnapshotManager;
using pgcpp::transaction::InitializeTransactionSystem;
using pgcpp::transaction::ResetTransactionState;
using pgcpp::types::kInt4Oid;

namespace {

class TypeDomainCastTest : public ::testing::Test {
protected:
    void SetUp() override {
        pgcpp::error::InitErrorSubsystem();
        context_ = AllocSetContext::Create("type_domain_cast_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);
        syscache_ = new SysCache();
        SetSysCache(syscache_);

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();
        BeginTransactionBlock();

        test_dir_ = "/tmp/pgcpp_type_domain_cast_" + std::to_string(getpid());
        SetStorageBaseDir(test_dir_);
        RunShell("rm -rf " + test_dir_);

        InitBufferPool(64);
        InitializeRelcache();

        // Register the int4 type so CREATE DOMAIN and CREATE CAST can resolve
        // it by name. Bootstrap catalog does not populate pg_type.
        // The parser maps SQL `integer` to SystemTypeName("int4"), so the
        // catalog lookup uses the last name component "int4".
        auto* int4_type = makePallocNode<FormData_pg_type>();
        int4_type->oid = kInt4Oid;
        int4_type->typname = "int4";
        int4_type->typlen = 4;
        int4_type->typbyval = true;
        int4_type->typtype = TypeType::kBase;
        catalog_->InsertType(int4_type);
    }

    void TearDown() override {
        EndTransactionBlock();
        ResetRelcache();
        ShutdownBufferPool();
        smgrcloseall();
        RunShell("rm -rf " + test_dir_);

        SetSysCache(nullptr);
        SetCatalog(nullptr);
        delete syscache_;
        delete catalog_;

        ResetTransactionState();
        InitializeTransactionSystem();
        InitializeSnapshotManager();

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    Node* ParseUtilityStmt(const std::string& sql) {
        std::vector<RawStmt*> raw = raw_parser(sql);
        if (raw.empty())
            return nullptr;
        std::vector<Query*> queries = parse_analyze(raw, sql.c_str());
        if (queries.empty())
            return nullptr;
        return queries[0]->utility_stmt;
    }

    std::string RunUtility(const std::string& sql) {
        Node* stmt = ParseUtilityStmt(sql);
        if (stmt == nullptr)
            return "";
        return ProcessUtility(stmt, &sink_);
    }

    static void RunShell(const std::string& cmd) {
        int rc = std::system(cmd.c_str());
        (void)rc;
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
    SysCache* syscache_ = nullptr;
    std::string test_dir_;
    StringSink sink_;
};

// ===========================================================================
// CREATE TYPE ... AS ENUM
// ===========================================================================

TEST_F(TypeDomainCastTest, CreateEnumTypeReturnsCorrectTag) {
    EXPECT_EQ(RunUtility("CREATE TYPE color AS ENUM ('red', 'green', 'blue')"), "CREATE TYPE");
}

TEST_F(TypeDomainCastTest, CreateEnumTypeRegistersInCatalog) {
    RunUtility("CREATE TYPE mood AS ENUM ('happy', 'sad', 'neutral')");

    const FormData_pg_type* type = catalog_->GetTypeByName("mood");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->typname, "mood");
    EXPECT_EQ(type->typtype, TypeType::kEnum);
    EXPECT_EQ(type->typisdefined, true);
    // Labels are stored as comma-separated string in typdefault.
    EXPECT_EQ(type->typdefault, "happy,sad,neutral");
}

TEST_F(TypeDomainCastTest, CreateEnumTypeDuplicateErrors) {
    RunUtility("CREATE TYPE status AS ENUM ('active', 'inactive')");
    EXPECT_THROW(RunUtility("CREATE TYPE status AS ENUM ('a', 'b')"), pgcpp::error::PgException);
}

TEST_F(TypeDomainCastTest, CreateEnumTypeSingleLabel) {
    EXPECT_EQ(RunUtility("CREATE TYPE singleton AS ENUM ('only')"), "CREATE TYPE");
    const FormData_pg_type* type = catalog_->GetTypeByName("singleton");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->typdefault, "only");
}

TEST_F(TypeDomainCastTest, CreateCommandTagForEnumType) {
    Node* stmt = ParseUtilityStmt("CREATE TYPE priority AS ENUM ('high', 'low')");
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(pgcpp::protocol::CreateCommandTag(stmt), "CREATE TYPE");
}

// ===========================================================================
// CREATE DOMAIN
// ===========================================================================

TEST_F(TypeDomainCastTest, CreateDomainReturnsCorrectTag) {
    EXPECT_EQ(RunUtility("CREATE DOMAIN posint AS integer"), "CREATE DOMAIN");
}

TEST_F(TypeDomainCastTest, CreateDomainRegistersInCatalog) {
    RunUtility("CREATE DOMAIN posint AS integer");

    const FormData_pg_type* type = catalog_->GetTypeByName("posint");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->typname, "posint");
    EXPECT_EQ(type->typtype, TypeType::kDomain);
    EXPECT_EQ(type->typbasetype, kInt4Oid);
    EXPECT_EQ(type->typisdefined, true);
}

TEST_F(TypeDomainCastTest, CreateDomainNotNullConstraint) {
    RunUtility("CREATE DOMAIN notnull_int AS integer NOT NULL");

    const FormData_pg_type* type = catalog_->GetTypeByName("notnull_int");
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(type->typtype, TypeType::kDomain);
    EXPECT_TRUE(type->typnotnull);
}

TEST_F(TypeDomainCastTest, CreateDomainInheritsBaseTypeProperties) {
    RunUtility("CREATE DOMAIN my_int AS integer");

    const FormData_pg_type* type = catalog_->GetTypeByName("my_int");
    ASSERT_NE(type, nullptr);
    const FormData_pg_type* base = catalog_->GetTypeByName("int4");
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(type->typlen, base->typlen);
    EXPECT_EQ(type->typbyval, base->typbyval);
    EXPECT_EQ(type->typalign, base->typalign);
    EXPECT_EQ(type->typstorage, base->typstorage);
}

TEST_F(TypeDomainCastTest, CreateDomainWithUnknownBaseTypeErrors) {
    EXPECT_THROW(RunUtility("CREATE DOMAIN bad_domain AS nonexistent_type"),
                 pgcpp::error::PgException);
}

TEST_F(TypeDomainCastTest, CreateDomainDuplicateErrors) {
    RunUtility("CREATE DOMAIN dup_domain AS integer");
    EXPECT_THROW(RunUtility("CREATE DOMAIN dup_domain AS integer"), pgcpp::error::PgException);
}

// ===========================================================================
// CREATE CAST
// ===========================================================================

TEST_F(TypeDomainCastTest, CreateCastWithoutFunctionReturnsCorrectTag) {
    // First create an enum type to use as a cast target.
    RunUtility("CREATE TYPE my_enum AS ENUM ('a', 'b')");
    EXPECT_EQ(RunUtility("CREATE CAST (integer AS my_enum) WITHOUT FUNCTION"), "CREATE CAST");
}

TEST_F(TypeDomainCastTest, CreateCastWithoutFunctionRegistersInCatalog) {
    RunUtility("CREATE TYPE my_enum AS ENUM ('a', 'b')");
    RunUtility("CREATE CAST (integer AS my_enum) WITHOUT FUNCTION");

    const FormData_pg_type* int_type = catalog_->GetTypeByName("int4");
    const FormData_pg_type* enum_type = catalog_->GetTypeByName("my_enum");
    ASSERT_NE(int_type, nullptr);
    ASSERT_NE(enum_type, nullptr);

    const FormData_pg_cast* cast = catalog_->GetCast(int_type->oid, enum_type->oid);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castsource, int_type->oid);
    EXPECT_EQ(cast->casttarget, enum_type->oid);
    EXPECT_EQ(cast->castmethod, CastMethod::kBinary);
    EXPECT_EQ(cast->castcontext, CastContext::kExplicit);
}

TEST_F(TypeDomainCastTest, CreateCastAsAssignment) {
    RunUtility("CREATE TYPE target_type AS ENUM ('x', 'y')");
    RunUtility("CREATE CAST (integer AS target_type) WITHOUT FUNCTION AS ASSIGNMENT");

    const FormData_pg_type* int_type = catalog_->GetTypeByName("int4");
    const FormData_pg_type* target_type = catalog_->GetTypeByName("target_type");
    ASSERT_NE(int_type, nullptr);
    ASSERT_NE(target_type, nullptr);

    const FormData_pg_cast* cast = catalog_->GetCast(int_type->oid, target_type->oid);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castcontext, CastContext::kAssignment);
}

TEST_F(TypeDomainCastTest, CreateCastAsImplicit) {
    RunUtility("CREATE TYPE implicit_target AS ENUM ('p', 'q')");
    RunUtility("CREATE CAST (integer AS implicit_target) WITHOUT FUNCTION AS IMPLICIT");

    const FormData_pg_type* int_type = catalog_->GetTypeByName("int4");
    const FormData_pg_type* target_type = catalog_->GetTypeByName("implicit_target");
    ASSERT_NE(int_type, nullptr);
    ASSERT_NE(target_type, nullptr);

    const FormData_pg_cast* cast = catalog_->GetCast(int_type->oid, target_type->oid);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castcontext, CastContext::kImplicit);
}

TEST_F(TypeDomainCastTest, CreateCastWithFunction) {
    RunUtility("CREATE TYPE func_target AS ENUM ('m', 'n')");
    RunUtility("CREATE CAST (integer AS func_target) WITH FUNCTION abs");

    const FormData_pg_type* int_type = catalog_->GetTypeByName("int4");
    const FormData_pg_type* target_type = catalog_->GetTypeByName("func_target");
    ASSERT_NE(int_type, nullptr);
    ASSERT_NE(target_type, nullptr);

    const FormData_pg_cast* cast = catalog_->GetCast(int_type->oid, target_type->oid);
    ASSERT_NE(cast, nullptr);
    EXPECT_EQ(cast->castmethod, CastMethod::kFunction);
}

TEST_F(TypeDomainCastTest, CreateCastDuplicateErrors) {
    RunUtility("CREATE TYPE dup_target AS ENUM ('r', 's')");
    RunUtility("CREATE CAST (integer AS dup_target) WITHOUT FUNCTION");
    EXPECT_THROW(RunUtility("CREATE CAST (integer AS dup_target) WITHOUT FUNCTION"),
                 pgcpp::error::PgException);
}

TEST_F(TypeDomainCastTest, CreateCastWithUnknownSourceErrors) {
    RunUtility("CREATE TYPE valid_target AS ENUM ('t', 'u')");
    EXPECT_THROW(RunUtility("CREATE CAST (unknown_type AS valid_target) WITHOUT FUNCTION"),
                 pgcpp::error::PgException);
}

// ===========================================================================
// Combined: CREATE TYPE → CREATE DOMAIN → CREATE CAST
// ===========================================================================

TEST_F(TypeDomainCastTest, CreateTypeDomainCastChain) {
    // Create an enum type.
    RunUtility("CREATE TYPE priority AS ENUM ('high', 'medium', 'low')");
    // Create a domain over integer.
    RunUtility("CREATE DOMAIN priority_level AS integer");
    // Create a cast from integer to the enum type.
    RunUtility("CREATE CAST (integer AS priority) WITHOUT FUNCTION");

    // Verify all three exist in the catalog.
    EXPECT_NE(catalog_->GetTypeByName("priority"), nullptr);
    EXPECT_NE(catalog_->GetTypeByName("priority_level"), nullptr);

    const FormData_pg_type* int_type = catalog_->GetTypeByName("int4");
    const FormData_pg_type* enum_type = catalog_->GetTypeByName("priority");
    ASSERT_NE(int_type, nullptr);
    ASSERT_NE(enum_type, nullptr);
    EXPECT_NE(catalog_->GetCast(int_type->oid, enum_type->oid), nullptr);
}

}  // namespace
