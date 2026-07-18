// pg_amcheck.cpp — main entry point for the pg_amcheck tool.
//
// Usage:
//   pgcpp_pg_amcheck [opts]
// Options:
//   -h, --host=HOST        server host (default: localhost)
//   -p, --port=PORT        server port (default: 5432)
//   -d, --dbname=NAME      database to check (default: connect DB)
//   -a, --all              check all databases
//   -i, --index            run bt_index_check (default: on)
//   -P, --parent           run bt_index_parent_check (heavier)
//   -H, --heapall          check heap relations (default: on)
//   -t, --table=PAT        only check tables whose name matches PAT
//   -I, --index-pattern=PAT  only check indexes matching PAT
//   -v, --verbose          print one line per relation
#include <iostream>
#include <string>

#include "tools/pg_amcheck.hpp"

namespace {

void PrintUsage() {
    std::cerr << "Usage: pg_amcheck [opts]\n"
              << "  -h, --host=HOST      server host\n"
              << "  -p, --port=PORT      server port\n"
              << "  -d, --dbname=NAME    database to check\n"
              << "  -a, --all            check all databases\n"
              << "  -i, --index          run bt_index_check (default)\n"
              << "  -P, --parent         run bt_index_parent_check\n"
              << "  -H, --heapall        check heap relations (default)\n"
              << "  -t, --table=PAT      table name substring filter\n"
              << "  -I, --index-pattern=PAT  index name substring filter\n"
              << "  -v, --verbose        per-relation output\n";
}

// MatchArg — returns the value if `argv[i]` starts with `prefix`, else nullopt.
bool MatchOpt(const std::string& arg, const std::string& short_name,
              const std::string& long_name, std::string* value) {
    std::string short_eq = short_name + "=";
    std::string long_eq = long_name + "=";
    if (arg == short_name || arg == long_name) {
        return true;  // boolean flag
    }
    if (arg.compare(0, short_eq.size(), short_eq) == 0) {
        *value = arg.substr(short_eq.size());
        return true;
    }
    if (arg.compare(0, long_eq.size(), long_eq) == 0) {
        *value = arg.substr(long_eq.size());
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    pgcpp::tools::AmcheckOptions opts;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::string val;
        if (a == "-h" || a == "--help") {
            PrintUsage();
            return 0;
        } else if (MatchOpt(a, "-h", "--host", &val)) {
            if (!val.empty())
                opts.host = val;
        } else if (MatchOpt(a, "-p", "--port", &val)) {
            if (!val.empty())
                opts.port = std::stoi(val);
        } else if (MatchOpt(a, "-d", "--dbname", &val)) {
            if (!val.empty())
                opts.dbname = val;
        } else if (a == "-a" || a == "--all") {
            opts.all_db = true;
        } else if (a == "-i" || a == "--index") {
            opts.index_check = true;
        } else if (a == "-P" || a == "--parent") {
            opts.parent_check = true;
        } else if (a == "-H" || a == "--heapall") {
            opts.heapall = true;
        } else if (MatchOpt(a, "-t", "--table", &val)) {
            if (!val.empty())
                opts.table_pattern = val;
        } else if (MatchOpt(a, "-I", "--index-pattern", &val)) {
            if (!val.empty())
                opts.index_pattern = val;
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            PrintUsage();
            return 2;
        }
    }

    if (opts.dbname.empty() && !opts.all_db) {
        std::cerr << "pg_amcheck: -d/--dbname is required (or use -a/--all)\n";
        return 2;
    }

    pgcpp::tools::AmcheckStats stats;
    std::ostream* vout = verbose ? &std::cout : nullptr;
    pgcpp::tools::AmcheckResult r = pgcpp::tools::RunAmcheck(opts, stats, vout);

    std::cout << "databases: " << stats.databases_checked
              << ", relations: " << stats.relations_checked
              << ", indexes: " << stats.indexes_checked
              << ", corrupt: " << stats.corrupt
              << ", errors: " << stats.errors << "\n";

    switch (r) {
        case pgcpp::tools::AmcheckResult::kOk:
            return 0;
        case pgcpp::tools::AmcheckResult::kCorruptionFound:
            return 1;
        case pgcpp::tools::AmcheckResult::kConnectFailed:
            std::cerr << "pg_amcheck: connection failed\n";
            return 2;
        case pgcpp::tools::AmcheckResult::kAmcheckExtensionMissing:
            std::cerr << "pg_amcheck: amcheck extension not available\n";
            return 2;
        case pgcpp::tools::AmcheckResult::kCatalogQueryFailed:
            std::cerr << "pg_amcheck: catalog query failed\n";
            return 2;
        case pgcpp::tools::AmcheckResult::kNoRelationsFound:
            std::cerr << "pg_amcheck: no relations to check\n";
            return 0;
    }
    return 0;
}
