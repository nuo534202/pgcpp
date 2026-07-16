// extension_test.cpp — Unit tests for CREATE/DROP EXTENSION (P3-10).
//
// Tests the extension mechanism end-to-end:
//   1. Control file parsing (key=value format, comments, booleans, requires)
//   2. ExtensionRegistry (register, lookup, list, clear)
//   3. RegisterBuiltinExtensions (3 built-in extensions)
//   4. pg_extension catalog CRUD (insert, lookup, delete)
//   5. CREATE EXTENSION command (basic, IF NOT EXISTS, CASCADE, errors)
//   6. DROP EXTENSION command (basic, IF EXISTS, multiple, errors)
//   7. Parser node Clone/Equals for CreateExtensionStmt / DropExtensionStmt
#include "extension/extension.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "catalog/bootstrap_catalog.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_extension.hpp"
#include "commands/extensioncmds.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"

namespace {

using pgcpp::catalog::BootstrapCatalog;
using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_extension;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::Oid;
using pgcpp::catalog::SetCatalog;
using pgcpp::commands::CreateExtension;
using pgcpp::commands::DropExtension;
using pgcpp::error::InitErrorSubsystem;
using pgcpp::error::PgException;
using pgcpp::extension::ExtensionControlFile;
using pgcpp::extension::ExtensionRegistry;
using pgcpp::extension::ExtensionScript;
using pgcpp::extension::GetExtensionRegistry;
using pgcpp::extension::ParseControlFile;
using pgcpp::extension::ParseControlFileWithName;
using pgcpp::extension::RegisterBuiltinExtensions;
using pgcpp::memory::AllocSetContext;
using pgcpp::nodes::makePallocNode;
using pgcpp::nodes::Node;
using pgcpp::parser::CreateExtensionStmt;
using pgcpp::parser::DropExtensionStmt;

// =============================================================================
// Test fixture: sets up memory context, error subsystem, catalog, and a clean
// extension registry for each test.
// =============================================================================
class ExtensionTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitErrorSubsystem();
        context_ = AllocSetContext::Create("extension_test_context");
        pgcpp::memory::SetCurrentMemoryContext(context_);

        catalog_ = new Catalog();
        SetCatalog(catalog_);
        BootstrapCatalog(catalog_);

        // Start with a clean registry for each test.
        GetExtensionRegistry().Clear();
    }

    void TearDown() override {
        SetCatalog(nullptr);
        delete catalog_;

        pgcpp::memory::SetCurrentMemoryContext(nullptr);
        if (context_ != nullptr) {
            context_->Delete();
        }
    }

    AllocSetContext* context_ = nullptr;
    Catalog* catalog_ = nullptr;
};

// =============================================================================
// 1. Control file parsing
// =============================================================================

TEST_F(ExtensionTest, ParseControlFile_BasicKeyValue) {
    std::string content =
        "default_version = '1.0'\n"
        "comment = 'test extension'\n"
        "relocatable = false\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFileWithName("myext", content, ctrl));
    EXPECT_EQ(ctrl.name, "myext");
    EXPECT_EQ(ctrl.default_version, "1.0");
    EXPECT_EQ(ctrl.comment, "test extension");
    EXPECT_FALSE(ctrl.relocatable);
}

TEST_F(ExtensionTest, ParseControlFile_CommentsAndBlankLines) {
    std::string content =
        "# this is a comment\n"
        "\n"
        "default_version = 1.0\n"
        "# another comment\n"
        "relocatable = true\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    EXPECT_EQ(ctrl.default_version, "1.0");
    EXPECT_TRUE(ctrl.relocatable);
}

TEST_F(ExtensionTest, ParseControlFile_UnquotedStrings) {
    std::string content =
        "default_version = 1.0\n"
        "module_pathname = $libdir/myext\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    EXPECT_EQ(ctrl.default_version, "1.0");
    EXPECT_EQ(ctrl.module_pathname, "$libdir/myext");
}

TEST_F(ExtensionTest, ParseControlFile_QuotedStrings) {
    std::string content =
        "comment = 'a quoted comment'\n"
        "schema = 'myschema'\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    EXPECT_EQ(ctrl.comment, "a quoted comment");
    EXPECT_EQ(ctrl.schema, "myschema");
}

TEST_F(ExtensionTest, ParseControlFile_Booleans) {
    std::string content =
        "relocatable = true\n"
        "superuser = false\n"
        "trusted = true\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    EXPECT_TRUE(ctrl.relocatable);
    EXPECT_FALSE(ctrl.superuser);
    EXPECT_TRUE(ctrl.trusted);
}

TEST_F(ExtensionTest, ParseControlFile_RequiresList) {
    std::string content = "requires = 'uuid, intarray'\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    ASSERT_EQ(ctrl.requires_list.size(), 2u);
    EXPECT_EQ(ctrl.requires_list[0], "uuid");
    EXPECT_EQ(ctrl.requires_list[1], "intarray");
}

TEST_F(ExtensionTest, ParseControlFile_RequiresSingle) {
    std::string content = "requires = 'uuid'\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    ASSERT_EQ(ctrl.requires_list.size(), 1u);
    EXPECT_EQ(ctrl.requires_list[0], "uuid");
}

TEST_F(ExtensionTest, ParseControlFile_UnknownKeysIgnored) {
    std::string content =
        "default_version = '1.0'\n"
        "unknown_key = 'some value'\n"
        "another_unknown = true\n";
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile(content, ctrl));
    EXPECT_EQ(ctrl.default_version, "1.0");
}

TEST_F(ExtensionTest, ParseControlFile_EmptyContent) {
    ExtensionControlFile ctrl{};
    ASSERT_TRUE(ParseControlFile("", ctrl));
    EXPECT_TRUE(ctrl.default_version.empty());
}

TEST_F(ExtensionTest, ParseControlFile_NoEquals) {
    std::string content = "this line has no equals sign\n";
    ExtensionControlFile ctrl{};
    // Malformed line should cause parse failure.
    EXPECT_FALSE(ParseControlFile(content, ctrl));
}

// =============================================================================
// 2. ExtensionRegistry
// =============================================================================

TEST_F(ExtensionTest, Registry_RegisterAndGetControlFile) {
    ExtensionRegistry reg;
    ExtensionControlFile ctrl{};
    ctrl.name = "myext";
    ctrl.default_version = "1.0";
    ASSERT_TRUE(reg.RegisterControlFile(ctrl));

    const auto* found = reg.GetControlFile("myext");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "myext");
    EXPECT_EQ(found->default_version, "1.0");
}

TEST_F(ExtensionTest, Registry_GetControlFileNotFound) {
    ExtensionRegistry reg;
    EXPECT_EQ(reg.GetControlFile("nonexistent"), nullptr);
}

TEST_F(ExtensionTest, Registry_ReplaceOnDuplicate) {
    ExtensionRegistry reg;
    ExtensionControlFile ctrl1{};
    ctrl1.name = "myext";
    ctrl1.default_version = "1.0";
    ctrl1.comment = "first version";
    ASSERT_TRUE(reg.RegisterControlFile(ctrl1));

    ExtensionControlFile ctrl2{};
    ctrl2.name = "myext";
    ctrl2.default_version = "2.0";
    ctrl2.comment = "second version";
    ASSERT_TRUE(reg.RegisterControlFile(ctrl2));

    // Should have replaced, not added.
    EXPECT_EQ(reg.Count(), 1u);
    const auto* found = reg.GetControlFile("myext");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->default_version, "2.0");
    EXPECT_EQ(found->comment, "second version");
}

TEST_F(ExtensionTest, Registry_RegisterAndGetScript) {
    ExtensionRegistry reg;
    ExtensionScript script{};
    script.extname = "myext";
    script.version = "1.0";
    script.sql = "CREATE FUNCTION foo() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql;";
    reg.RegisterScript(script);

    const auto* found = reg.GetScript("myext", "1.0");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->extname, "myext");
    EXPECT_EQ(found->version, "1.0");
    EXPECT_EQ(found->sql, script.sql);
}

TEST_F(ExtensionTest, Registry_GetScriptNotFound) {
    ExtensionRegistry reg;
    EXPECT_EQ(reg.GetScript("myext", "1.0"), nullptr);
}

TEST_F(ExtensionTest, Registry_ListAvailable) {
    ExtensionRegistry reg;
    ExtensionControlFile ctrl1{};
    ctrl1.name = "ext_a";
    ctrl1.default_version = "1.0";
    ExtensionControlFile ctrl2{};
    ctrl2.name = "ext_b";
    ctrl2.default_version = "1.0";
    reg.RegisterControlFile(ctrl1);
    reg.RegisterControlFile(ctrl2);

    auto names = reg.ListAvailable();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(ExtensionTest, Registry_Clear) {
    ExtensionRegistry reg;
    ExtensionControlFile ctrl{};
    ctrl.name = "myext";
    ctrl.default_version = "1.0";
    reg.RegisterControlFile(ctrl);
    EXPECT_EQ(reg.Count(), 1u);

    reg.Clear();
    EXPECT_EQ(reg.Count(), 0u);
    EXPECT_EQ(reg.GetControlFile("myext"), nullptr);
}

TEST_F(ExtensionTest, Registry_Singleton) {
    ExtensionRegistry& reg1 = GetExtensionRegistry();
    ExtensionRegistry& reg2 = GetExtensionRegistry();
    EXPECT_EQ(&reg1, &reg2);
}

// =============================================================================
// 3. RegisterBuiltinExtensions
// =============================================================================

TEST_F(ExtensionTest, BuiltinExtensions_Registered) {
    RegisterBuiltinExtensions();
    auto& reg = GetExtensionRegistry();
    EXPECT_EQ(reg.Count(), 3u);

    auto names = reg.ListAvailable();
    EXPECT_EQ(names.size(), 3u);
}

TEST_F(ExtensionTest, BuiltinExtensions_PgcppUuid) {
    RegisterBuiltinExtensions();
    const auto* ctrl = GetExtensionRegistry().GetControlFile("pgcpp_uuid");
    ASSERT_NE(ctrl, nullptr);
    EXPECT_EQ(ctrl->name, "pgcpp_uuid");
    EXPECT_TRUE(ctrl->relocatable);
    EXPECT_TRUE(ctrl->requires_list.empty());
}

TEST_F(ExtensionTest, BuiltinExtensions_PgcppIntarray) {
    RegisterBuiltinExtensions();
    const auto* ctrl = GetExtensionRegistry().GetControlFile("pgcpp_intarray");
    ASSERT_NE(ctrl, nullptr);
    EXPECT_EQ(ctrl->name, "pgcpp_intarray");
    EXPECT_FALSE(ctrl->relocatable);
    EXPECT_EQ(ctrl->schema, "public");
}

TEST_F(ExtensionTest, BuiltinExtensions_PgcppTestRequiresUuid) {
    RegisterBuiltinExtensions();
    const auto* ctrl = GetExtensionRegistry().GetControlFile("pgcpp_test");
    ASSERT_NE(ctrl, nullptr);
    EXPECT_EQ(ctrl->name, "pgcpp_test");
    ASSERT_EQ(ctrl->requires_list.size(), 1u);
    EXPECT_EQ(ctrl->requires_list[0], "pgcpp_uuid");
}

// =============================================================================
// 4. pg_extension catalog CRUD
// =============================================================================

TEST_F(ExtensionTest, Catalog_InsertAndGetByOid) {
    auto* row = makePallocNode<FormData_pg_extension>();
    row->extname = "test_ext";
    row->extversion = "1.0";
    row->extrelocatable = true;

    Oid oid = GetCatalog()->InsertExtension(row);
    EXPECT_NE(oid, kInvalidOid);

    const auto* found = GetCatalog()->GetExtensionByOid(oid);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->extname, "test_ext");
    EXPECT_EQ(found->extversion, "1.0");
    EXPECT_TRUE(found->extrelocatable);
}

TEST_F(ExtensionTest, Catalog_GetByOidNotFound) {
    EXPECT_EQ(GetCatalog()->GetExtensionByOid(99999), nullptr);
}

TEST_F(ExtensionTest, Catalog_GetByName) {
    auto* row = makePallocNode<FormData_pg_extension>();
    row->extname = "named_ext";
    row->extversion = "2.0";
    GetCatalog()->InsertExtension(row);

    const auto* found = GetCatalog()->GetExtensionByName("named_ext");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->extname, "named_ext");
    EXPECT_EQ(found->extversion, "2.0");
}

TEST_F(ExtensionTest, Catalog_GetByNameNotFound) {
    EXPECT_EQ(GetCatalog()->GetExtensionByName("nonexistent"), nullptr);
}

TEST_F(ExtensionTest, Catalog_GetByNameCaseSensitive) {
    auto* row = makePallocNode<FormData_pg_extension>();
    row->extname = "MyExt";
    GetCatalog()->InsertExtension(row);

    EXPECT_NE(GetCatalog()->GetExtensionByName("MyExt"), nullptr);
    EXPECT_EQ(GetCatalog()->GetExtensionByName("myext"), nullptr);
}

TEST_F(ExtensionTest, Catalog_GetAllExtensions) {
    auto* row1 = makePallocNode<FormData_pg_extension>();
    row1->extname = "ext1";
    GetCatalog()->InsertExtension(row1);

    auto* row2 = makePallocNode<FormData_pg_extension>();
    row2->extname = "ext2";
    GetCatalog()->InsertExtension(row2);

    auto all = GetCatalog()->GetAllExtensions();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(ExtensionTest, Catalog_GetAllExtensionsEmpty) {
    auto all = GetCatalog()->GetAllExtensions();
    EXPECT_TRUE(all.empty());
}

TEST_F(ExtensionTest, Catalog_DeleteExtension) {
    auto* row = makePallocNode<FormData_pg_extension>();
    row->extname = "to_delete";
    Oid oid = GetCatalog()->InsertExtension(row);
    EXPECT_NE(oid, kInvalidOid);

    EXPECT_TRUE(GetCatalog()->DeleteExtension(oid));
    EXPECT_EQ(GetCatalog()->GetExtensionByOid(oid), nullptr);
    EXPECT_EQ(GetCatalog()->GetExtensionByName("to_delete"), nullptr);
}

TEST_F(ExtensionTest, Catalog_DeleteExtensionNotFound) {
    EXPECT_FALSE(GetCatalog()->DeleteExtension(99999));
}

// =============================================================================
// 5. CREATE EXTENSION command
// =============================================================================

TEST_F(ExtensionTest, CreateExtension_Basic) {
    RegisterBuiltinExtensions();

    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "pgcpp_uuid";

    std::string tag = CreateExtension(stmt);
    EXPECT_EQ(tag, "CREATE EXTENSION");

    // Verify the pg_extension row was inserted.
    const auto* row = GetCatalog()->GetExtensionByName("pgcpp_uuid");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->extname, "pgcpp_uuid");
    EXPECT_FALSE(row->extversion.empty());
    EXPECT_TRUE(row->extrelocatable);
}

TEST_F(ExtensionTest, CreateExtension_AlreadyExists) {
    RegisterBuiltinExtensions();

    auto* stmt1 = makePallocNode<CreateExtensionStmt>();
    stmt1->extname = "pgcpp_uuid";
    CreateExtension(stmt1);

    auto* stmt2 = makePallocNode<CreateExtensionStmt>();
    stmt2->extname = "pgcpp_uuid";
    stmt2->if_not_exists = false;

    EXPECT_THROW({ CreateExtension(stmt2); }, PgException);
}

TEST_F(ExtensionTest, CreateExtension_IfNotExists) {
    RegisterBuiltinExtensions();

    auto* stmt1 = makePallocNode<CreateExtensionStmt>();
    stmt1->extname = "pgcpp_uuid";
    CreateExtension(stmt1);

    auto* stmt2 = makePallocNode<CreateExtensionStmt>();
    stmt2->extname = "pgcpp_uuid";
    stmt2->if_not_exists = true;

    // Should not throw, should not create a duplicate.
    std::string tag = CreateExtension(stmt2);
    EXPECT_EQ(tag, "CREATE EXTENSION");

    auto all = GetCatalog()->GetAllExtensions();
    EXPECT_EQ(all.size(), 1u);
}

TEST_F(ExtensionTest, CreateExtension_NotAvailable) {
    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "nonexistent_ext";

    EXPECT_THROW({ CreateExtension(stmt); }, PgException);
}

TEST_F(ExtensionTest, CreateExtension_CascadeInstallsDependencies) {
    RegisterBuiltinExtensions();

    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "pgcpp_test";
    stmt->cascade = true;

    std::string tag = CreateExtension(stmt);
    EXPECT_EQ(tag, "CREATE EXTENSION");

    // CASCADE should have installed the dependency (pgcpp_uuid) too.
    EXPECT_NE(GetCatalog()->GetExtensionByName("pgcpp_test"), nullptr);
    EXPECT_NE(GetCatalog()->GetExtensionByName("pgcpp_uuid"), nullptr);

    auto all = GetCatalog()->GetAllExtensions();
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(ExtensionTest, CreateExtension_NoCascadeMissingDependency) {
    // Register pgcpp_test which requires pgcpp_uuid, but don't CASCADE.
    // The extension itself is installed; dependencies are only installed
    // with CASCADE. (This is a simplified behavior — PostgreSQL would error
    // here, but pgcpp's simplified impl only installs deps with CASCADE.)
    RegisterBuiltinExtensions();

    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "pgcpp_test";
    stmt->cascade = false;

    std::string tag = CreateExtension(stmt);
    EXPECT_EQ(tag, "CREATE EXTENSION");

    // pgcpp_test installed, pgcpp_uuid NOT installed (no CASCADE).
    EXPECT_NE(GetCatalog()->GetExtensionByName("pgcpp_test"), nullptr);
    EXPECT_EQ(GetCatalog()->GetExtensionByName("pgcpp_uuid"), nullptr);
}

TEST_F(ExtensionTest, CreateExtension_EmptyName) {
    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "";

    EXPECT_THROW({ CreateExtension(stmt); }, PgException);
}

TEST_F(ExtensionTest, CreateExtension_NullStmt) {
    std::string tag = CreateExtension(nullptr);
    EXPECT_EQ(tag, "CREATE EXTENSION");
}

TEST_F(ExtensionTest, CreateExtension_VersionResolution) {
    RegisterBuiltinExtensions();

    auto* stmt = makePallocNode<CreateExtensionStmt>();
    stmt->extname = "pgcpp_uuid";
    // Don't specify version — should use default_version from control file.

    CreateExtension(stmt);

    const auto* row = GetCatalog()->GetExtensionByName("pgcpp_uuid");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->extversion.empty());
}

// =============================================================================
// 6. DROP EXTENSION command
// =============================================================================

TEST_F(ExtensionTest, DropExtension_Basic) {
    RegisterBuiltinExtensions();

    auto* create = makePallocNode<CreateExtensionStmt>();
    create->extname = "pgcpp_uuid";
    CreateExtension(create);

    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_uuid");

    std::string tag = DropExtension(drop);
    EXPECT_EQ(tag, "DROP EXTENSION");

    EXPECT_EQ(GetCatalog()->GetExtensionByName("pgcpp_uuid"), nullptr);
}

TEST_F(ExtensionTest, DropExtension_NotExists) {
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("nonexistent");

    EXPECT_THROW({ DropExtension(drop); }, PgException);
}

TEST_F(ExtensionTest, DropExtension_IfExists) {
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("nonexistent");
    drop->missing_ok = true;

    std::string tag = DropExtension(drop);
    EXPECT_EQ(tag, "DROP EXTENSION");
}

TEST_F(ExtensionTest, DropExtension_MultipleNames) {
    RegisterBuiltinExtensions();

    // Create two extensions.
    auto* c1 = makePallocNode<CreateExtensionStmt>();
    c1->extname = "pgcpp_uuid";
    CreateExtension(c1);

    auto* c2 = makePallocNode<CreateExtensionStmt>();
    c2->extname = "pgcpp_intarray";
    CreateExtension(c2);

    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 2u);

    // Drop both in one command.
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_uuid");
    drop->extnames.push_back("pgcpp_intarray");

    std::string tag = DropExtension(drop);
    EXPECT_EQ(tag, "DROP EXTENSION");

    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 0u);
}

TEST_F(ExtensionTest, DropExtension_MultipleWithMissing) {
    RegisterBuiltinExtensions();

    auto* c1 = makePallocNode<CreateExtensionStmt>();
    c1->extname = "pgcpp_uuid";
    CreateExtension(c1);

    // Drop existing + non-existing without IF EXISTS — should error.
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_uuid");
    drop->extnames.push_back("nonexistent");

    EXPECT_THROW({ DropExtension(drop); }, PgException);
    // The first extension may or may not be deleted depending on iteration
    // order; PostgreSQL deletes atomically. pgcpp deletes as it iterates.
}

TEST_F(ExtensionTest, DropExtension_MultipleWithIfExists) {
    RegisterBuiltinExtensions();

    auto* c1 = makePallocNode<CreateExtensionStmt>();
    c1->extname = "pgcpp_uuid";
    CreateExtension(c1);

    // Drop existing + non-existing with IF EXISTS — should skip missing.
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_uuid");
    drop->extnames.push_back("nonexistent");
    drop->missing_ok = true;

    std::string tag = DropExtension(drop);
    EXPECT_EQ(tag, "DROP EXTENSION");

    EXPECT_EQ(GetCatalog()->GetExtensionByName("pgcpp_uuid"), nullptr);
}

TEST_F(ExtensionTest, DropExtension_NullStmt) {
    std::string tag = DropExtension(nullptr);
    EXPECT_EQ(tag, "DROP EXTENSION");
}

TEST_F(ExtensionTest, DropExtension_EmptyNameList) {
    auto* drop = makePallocNode<DropExtensionStmt>();
    // No names in the list.

    std::string tag = DropExtension(drop);
    EXPECT_EQ(tag, "DROP EXTENSION");
}

// =============================================================================
// 7. Parser node Clone/Equals
// =============================================================================

TEST_F(ExtensionTest, CreateExtensionStmt_Clone) {
    auto* orig = makePallocNode<CreateExtensionStmt>();
    orig->if_not_exists = true;
    orig->extname = "myext";
    orig->schema = "public";
    orig->version = "1.0";
    orig->cascade = true;

    Node* copy = orig->Clone();
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->GetTag(), orig->GetTag());

    auto* cloned = static_cast<CreateExtensionStmt*>(copy);
    EXPECT_EQ(cloned->if_not_exists, orig->if_not_exists);
    EXPECT_EQ(cloned->extname, orig->extname);
    EXPECT_EQ(cloned->schema, orig->schema);
    EXPECT_EQ(cloned->version, orig->version);
    EXPECT_EQ(cloned->cascade, orig->cascade);
}

TEST_F(ExtensionTest, CreateExtensionStmt_Equals) {
    auto* a = makePallocNode<CreateExtensionStmt>();
    a->if_not_exists = true;
    a->extname = "myext";
    a->schema = "public";
    a->version = "1.0";
    a->cascade = true;

    auto* b = makePallocNode<CreateExtensionStmt>();
    b->if_not_exists = true;
    b->extname = "myext";
    b->schema = "public";
    b->version = "1.0";
    b->cascade = true;

    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(ExtensionTest, CreateExtensionStmt_NotEqualsDifferentName) {
    auto* a = makePallocNode<CreateExtensionStmt>();
    a->extname = "ext_a";

    auto* b = makePallocNode<CreateExtensionStmt>();
    b->extname = "ext_b";

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, CreateExtensionStmt_NotEqualsDifferentIfNotExists) {
    auto* a = makePallocNode<CreateExtensionStmt>();
    a->extname = "myext";
    a->if_not_exists = true;

    auto* b = makePallocNode<CreateExtensionStmt>();
    b->extname = "myext";
    b->if_not_exists = false;

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, CreateExtensionStmt_NotEqualsDifferentCascade) {
    auto* a = makePallocNode<CreateExtensionStmt>();
    a->extname = "myext";
    a->cascade = true;

    auto* b = makePallocNode<CreateExtensionStmt>();
    b->extname = "myext";
    b->cascade = false;

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, CreateExtensionStmt_NotEqualsDifferentTag) {
    auto* a = makePallocNode<CreateExtensionStmt>();
    a->extname = "myext";

    auto* b = makePallocNode<DropExtensionStmt>();
    b->extnames.push_back("myext");

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, DropExtensionStmt_Clone) {
    auto* orig = makePallocNode<DropExtensionStmt>();
    orig->extnames = {"ext1", "ext2", "ext3"};
    orig->missing_ok = true;
    orig->cascade = false;

    Node* copy = orig->Clone();
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->GetTag(), orig->GetTag());

    auto* cloned = static_cast<DropExtensionStmt*>(copy);
    EXPECT_EQ(cloned->extnames, orig->extnames);
    EXPECT_EQ(cloned->missing_ok, orig->missing_ok);
    EXPECT_EQ(cloned->cascade, orig->cascade);
}

TEST_F(ExtensionTest, DropExtensionStmt_Equals) {
    auto* a = makePallocNode<DropExtensionStmt>();
    a->extnames = {"ext1", "ext2"};
    a->missing_ok = true;
    a->cascade = false;

    auto* b = makePallocNode<DropExtensionStmt>();
    b->extnames = {"ext1", "ext2"};
    b->missing_ok = true;
    b->cascade = false;

    EXPECT_TRUE(a->Equals(*b));
}

TEST_F(ExtensionTest, DropExtensionStmt_NotEqualsDifferentNames) {
    auto* a = makePallocNode<DropExtensionStmt>();
    a->extnames = {"ext1", "ext2"};

    auto* b = makePallocNode<DropExtensionStmt>();
    b->extnames = {"ext1", "ext3"};

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, DropExtensionStmt_NotEqualsDifferentNameCount) {
    auto* a = makePallocNode<DropExtensionStmt>();
    a->extnames = {"ext1", "ext2"};

    auto* b = makePallocNode<DropExtensionStmt>();
    b->extnames = {"ext1"};

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, DropExtensionStmt_NotEqualsDifferentMissingOk) {
    auto* a = makePallocNode<DropExtensionStmt>();
    a->extnames = {"ext1"};
    a->missing_ok = true;

    auto* b = makePallocNode<DropExtensionStmt>();
    b->extnames = {"ext1"};
    b->missing_ok = false;

    EXPECT_FALSE(a->Equals(*b));
}

TEST_F(ExtensionTest, DropExtensionStmt_NotEqualsDifferentTag) {
    auto* a = makePallocNode<DropExtensionStmt>();
    a->extnames = {"myext"};

    auto* b = makePallocNode<CreateExtensionStmt>();
    b->extname = "myext";

    EXPECT_FALSE(a->Equals(*b));
}

// =============================================================================
// 8. End-to-end: CREATE then DROP
// =============================================================================

TEST_F(ExtensionTest, EndToEnd_CreateAndDrop) {
    RegisterBuiltinExtensions();

    // CREATE
    auto* create = makePallocNode<CreateExtensionStmt>();
    create->extname = "pgcpp_uuid";
    std::string create_tag = CreateExtension(create);
    EXPECT_EQ(create_tag, "CREATE EXTENSION");
    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 1u);

    // DROP
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_uuid");
    std::string drop_tag = DropExtension(drop);
    EXPECT_EQ(drop_tag, "DROP EXTENSION");
    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 0u);

    // CREATE again (should work after DROP)
    std::string create_tag2 = CreateExtension(create);
    EXPECT_EQ(create_tag2, "CREATE EXTENSION");
    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 1u);
}

TEST_F(ExtensionTest, EndToEnd_CascadeCreateAndDrop) {
    RegisterBuiltinExtensions();

    // CREATE with CASCADE (installs pgcpp_test + pgcpp_uuid)
    auto* create = makePallocNode<CreateExtensionStmt>();
    create->extname = "pgcpp_test";
    create->cascade = true;
    CreateExtension(create);
    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 2u);

    // DROP both
    auto* drop = makePallocNode<DropExtensionStmt>();
    drop->extnames.push_back("pgcpp_test");
    drop->extnames.push_back("pgcpp_uuid");
    DropExtension(drop);
    EXPECT_EQ(GetCatalog()->GetAllExtensions().size(), 0u);
}

}  // namespace
