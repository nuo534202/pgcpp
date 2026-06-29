// lsyscache.cpp — low-level syscache convenience lookups.
//
// Converted from PostgreSQL 15's src/backend/utils/cache/lsyscache.c.
//
// Each function is a thin wrapper over the global Catalog (Catalog::GetCatalog),
// returning default values (InvalidOid / nullptr / false) on miss and never
// ereport(ERROR). String results are palloc'd in the current memory context.
#include "catalog/lsyscache.hpp"

#include <cstring>
#include <string>

#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_type.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::catalog {

namespace {

// palloc_str — allocate a palloc'd char* copy of a std::string.
// Returns nullptr if the input is empty AND len is zero (PG returns NULL for
// empty names in some callers, but we preserve the actual string content).
char* palloc_str(const std::string& s) {
    char* out = static_cast<char*>(pgcpp::memory::palloc(s.size() + 1));
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

}  // namespace

// --- pg_operator lookups ---

Oid get_opcode(Oid opoid) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr) {
        return kInvalidOid;
    }
    return op->oprcode;
}

const FormData_pg_operator* get_op(Oid opoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    return cat->GetOperatorByOid(opoid);
}

char* get_opname(Oid opoid) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr) {
        return nullptr;
    }
    return palloc_str(op->oprname);
}

Oid get_commutator(Oid opoid) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr) {
        return kInvalidOid;
    }
    return op->oprcom;
}

Oid get_negator(Oid opoid) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr) {
        return kInvalidOid;
    }
    return op->oprnegate;
}

bool op_mergejoinable(Oid opoid, Oid* left, Oid* right) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr || !op->oprcanmerge) {
        return false;
    }
    if (left != nullptr) {
        *left = op->oprleft;
    }
    if (right != nullptr) {
        *right = op->oprright;
    }
    return true;
}

bool op_strict(Oid opoid) {
    const FormData_pg_operator* op = get_op(opoid);
    if (op == nullptr || op->oprcode == kInvalidOid) {
        return false;
    }
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return false;
    }
    const FormData_pg_proc* proc = cat->GetProcByOid(op->oprcode);
    if (proc == nullptr) {
        return false;
    }
    return proc->proisstrict;
}

// --- pg_proc lookups ---

Oid get_func_rettype(Oid funcoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return kInvalidOid;
    }
    const FormData_pg_proc* proc = cat->GetProcByOid(funcoid);
    if (proc == nullptr) {
        return kInvalidOid;
    }
    return proc->prorettype;
}

char* get_func_name(Oid funcoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_proc* proc = cat->GetProcByOid(funcoid);
    if (proc == nullptr) {
        return nullptr;
    }
    return palloc_str(proc->proname);
}

char get_func_prokind(Oid funcoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_proc* proc = cat->GetProcByOid(funcoid);
    if (proc == nullptr) {
        return '\0';
    }
    return static_cast<char>(proc->prokind);
}

int get_func_nargs(Oid funcoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return -1;
    }
    const FormData_pg_proc* proc = cat->GetProcByOid(funcoid);
    if (proc == nullptr) {
        return -1;
    }
    return proc->pronargs;
}

// --- pg_type lookups ---

char* get_type_name(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return nullptr;
    }
    return palloc_str(type->typname);
}

int16_t get_typlen(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return 0;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return 0;
    }
    return type->typlen;
}

bool get_typbyval(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return false;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return false;
    }
    return type->typbyval;
}

char get_typalign(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return '\0';
    }
    return static_cast<char>(type->typalign);
}

char get_typstorage(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return '\0';
    }
    return static_cast<char>(type->typstorage);
}

char get_typcategory(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return '\0';
    }
    return static_cast<char>(type->typcategory);
}

bool get_typisdefined(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return false;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return false;
    }
    return type->typisdefined;
}

Oid get_typelem(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return kInvalidOid;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return kInvalidOid;
    }
    return type->typelem;
}

// --- pg_attribute lookups ---

const FormData_pg_attribute* get_att(Oid relid, const char* attname) {
    if (attname == nullptr) {
        return nullptr;
    }
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    auto attrs = cat->GetAttributes(relid);
    for (const auto* a : attrs) {
        if (a->attname == attname) {
            return a;
        }
    }
    return nullptr;
}

char* get_attname(Oid relid, int16_t attnum) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_attribute* attr = cat->GetAttribute(relid, attnum);
    if (attr == nullptr) {
        return nullptr;
    }
    return palloc_str(attr->attname);
}

Oid get_atttype(Oid relid, int16_t attnum) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return kInvalidOid;
    }
    const FormData_pg_attribute* attr = cat->GetAttribute(relid, attnum);
    if (attr == nullptr) {
        return kInvalidOid;
    }
    return attr->atttypid;
}

int16_t get_attnum(Oid relid, const char* attname) {
    const FormData_pg_attribute* attr = get_att(relid, attname);
    if (attr == nullptr) {
        return kInvalidAttrNumber;
    }
    return attr->attnum;
}

bool get_attnotnull(Oid relid, int16_t attnum) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return false;
    }
    const FormData_pg_attribute* attr = cat->GetAttribute(relid, attnum);
    if (attr == nullptr) {
        return false;
    }
    return attr->attnotnull;
}

// --- pg_class lookups ---

char* get_rel_name(Oid relid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_class* rel = cat->GetClassByOid(relid);
    if (rel == nullptr) {
        return nullptr;
    }
    return palloc_str(rel->relname);
}

char get_rel_relkind(Oid relid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_class* rel = cat->GetClassByOid(relid);
    if (rel == nullptr) {
        return '\0';
    }
    return static_cast<char>(rel->relkind);
}

char get_rel_persistence(Oid relid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return '\0';
    }
    const FormData_pg_class* rel = cat->GetClassByOid(relid);
    if (rel == nullptr) {
        return '\0';
    }
    return static_cast<char>(rel->relpersistence);
}

Oid get_rel_namespace(Oid relid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return kInvalidOid;
    }
    const FormData_pg_class* rel = cat->GetClassByOid(relid);
    if (rel == nullptr) {
        return kInvalidOid;
    }
    return rel->relnamespace;
}

// --- Convenience predicates ---

bool type_is_rowtype(Oid typoid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return false;
    }
    const FormData_pg_type* type = cat->GetTypeByOid(typoid);
    if (type == nullptr) {
        return false;
    }
    return type->typtype == TypeType::kComposite;
}

bool type_is_enum(Oid typoid) {
    // pgcpp does not yet model enum types; the bootstrap catalog contains
    // no enum rows. Preserve the predicate for API compatibility.
    (void)typoid;
    return false;
}

}  // namespace pgcpp::catalog
