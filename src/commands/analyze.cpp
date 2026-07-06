// analyze.cpp — ANALYZE command implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/analyze.c.
//
// Scans heap relations to collect per-column statistics (null fraction,
// average width, distinct count, most common values, histogram) and writes
// them to pg_statistic. Also updates pg_class.relpages/reltuples so the
// optimizer's cost model has accurate relation sizes.
//
// pgcpp simplification: we scan all rows (no reservoir sampling), compute
// exact statistics, and serialize MCV/histogram as comma-separated strings
// in stavalues1/stavalues2. Only int4 and text columns are analyzed; other
// types get nullfrac + width only.
#include "commands/analyze.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "access/heapam.hpp"
#include "access/rel.hpp"
#include "catalog/catalog.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_statistic.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/alloc_set.hpp"
#include "common/memory/memory_context.hpp"
#include "parser/parsenodes.hpp"
#include "storage/bufmgr.hpp"
#include "transaction/heap_tuple.hpp"
#include "transaction/snapshot.hpp"
#include "types/datum.hpp"

namespace pgcpp::commands {

using pgcpp::access::heap_beginscan;
using pgcpp::access::heap_endscan;
using pgcpp::access::heap_getattr;
using pgcpp::access::heap_getnext;
using pgcpp::access::HeapScanDesc;
using pgcpp::access::Relation;
using pgcpp::access::RelationClose;
using pgcpp::access::RelationGetNumberOfBlocks;
using pgcpp::access::RelationOpen;
using pgcpp::access::TupleDesc;
using pgcpp::catalog::FormData_pg_attribute;
using pgcpp::catalog::FormData_pg_class;
using pgcpp::catalog::FormData_pg_statistic;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::kStatisticKindHistogram;
using pgcpp::catalog::kStatisticKindMcv;
using pgcpp::catalog::Oid;
using pgcpp::catalog::RelKind;
using pgcpp::memory::palloc;
using pgcpp::nodes::makePallocNode;
using pgcpp::parser::Node;
using pgcpp::parser::RangeVar;
using pgcpp::parser::VacuumStmt;
using pgcpp::transaction::HeapTuple;
using pgcpp::transaction::Snapshot;
using pgcpp::types::Datum;
using pgcpp::types::DatumGetInt32;
using pgcpp::types::kInt4Oid;
using pgcpp::types::kTextOid;

namespace {

// Maximum number of MCV slots and histogram buckets (PostgreSQL default).
constexpr int kNumStatsSlots = 10;

// --- Value collection helpers ---

// Collect a column's values from a heap scan. Stores int32 values for int4
// columns and text values for text columns. Returns null count and total
// row count via out-params.
struct ColumnSamples {
    std::vector<int32_t> int_values;       // for int4 columns
    std::vector<std::string> text_values;  // for text columns
    int null_count = 0;
    int total_rows = 0;
    int width_sum = 0;  // sum of stored widths (for stawidth)
};

// Compute null fraction as a float between 0.0 and 1.0.
float ComputeNullFraction(int null_count, int total) {
    if (total == 0)
        return 0.0F;
    return static_cast<float>(null_count) / static_cast<float>(total);
}

// Compute average width (rounded to int).
int32_t ComputeAvgWidth(int width_sum, int total) {
    if (total == 0)
        return 0;
    return width_sum / total;
}

// Compute distinct count. Returns a positive integer (exact count).
int32_t ComputeDistinctInt(const std::vector<int32_t>& values) {
    std::vector<int32_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    return static_cast<int32_t>(sorted.size());
}

int32_t ComputeDistinctText(const std::vector<std::string>& values) {
    std::vector<std::string> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    return static_cast<int32_t>(sorted.size());
}

// Compute MCV for int32 values: top-K most frequent values.
// Returns parallel arrays of values and frequencies.
void ComputeMcvInt(const std::vector<int32_t>& values, int max_slots,
                   std::vector<int32_t>* mcv_values, std::vector<float>* mcv_freqs) {
    std::map<int32_t, int> freq_map;
    for (int32_t v : values)
        freq_map[v]++;

    // Sort by frequency (descending), then by value (ascending) for ties.
    std::vector<std::pair<int, int32_t>> sorted;
    sorted.reserve(freq_map.size());
    for (const auto& [v, cnt] : freq_map)
        sorted.emplace_back(cnt, v);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second < b.second;
    });

    int n = std::min(max_slots, static_cast<int>(sorted.size()));
    float total = static_cast<float>(values.size());
    for (int i = 0; i < n; i++) {
        mcv_values->push_back(sorted[i].second);
        mcv_freqs->push_back(total > 0.0F ? static_cast<float>(sorted[i].first) / total : 0.0F);
    }
}

// Compute MCV for text values.
void ComputeMcvText(const std::vector<std::string>& values, int max_slots,
                    std::vector<std::string>* mcv_values, std::vector<float>* mcv_freqs) {
    std::map<std::string, int> freq_map;
    for (const auto& v : values)
        freq_map[v]++;

    std::vector<std::pair<int, std::string>> sorted;
    sorted.reserve(freq_map.size());
    for (const auto& [v, cnt] : freq_map)
        sorted.emplace_back(cnt, v);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second < b.second;
    });

    int n = std::min(max_slots, static_cast<int>(sorted.size()));
    float total = static_cast<float>(values.size());
    for (int i = 0; i < n; i++) {
        mcv_values->push_back(sorted[i].second);
        mcv_freqs->push_back(total > 0.0F ? static_cast<float>(sorted[i].first) / total : 0.0F);
    }
}

// Compute histogram for int32 values (excluding MCV values).
// Returns sorted bucket boundary values (kNumStatsSlots - 1 boundaries would
// give kNumStatsSlots buckets, but PostgreSQL stores kNumStatsSlots values
// representing the sorted non-MCV values divided into equal-frequency buckets).
void ComputeHistogramInt(const std::vector<int32_t>& values, const std::vector<int32_t>& mcv_values,
                         std::vector<int32_t>* hist_values) {
    // Exclude MCV values from the histogram.
    std::set<int32_t> mcv_set(mcv_values.begin(), mcv_values.end());
    std::vector<int32_t> remaining;
    for (int32_t v : values) {
        if (mcv_set.find(v) == mcv_set.end())
            remaining.push_back(v);
    }
    std::sort(remaining.begin(), remaining.end());
    if (remaining.empty())
        return;

    // Divide into kNumStatsSlots equal-frequency buckets.
    // Store one representative value per bucket boundary.
    int n = static_cast<int>(remaining.size());
    int slots = std::min(kNumStatsSlots, n);
    for (int i = 0; i < slots; i++) {
        int idx = (n * i) / slots;
        hist_values->push_back(remaining[idx]);
    }
}

void ComputeHistogramText(const std::vector<std::string>& values,
                          const std::vector<std::string>& mcv_values,
                          std::vector<std::string>* hist_values) {
    std::set<std::string> mcv_set(mcv_values.begin(), mcv_values.end());
    std::vector<std::string> remaining;
    for (const auto& v : values) {
        if (mcv_set.find(v) == mcv_set.end())
            remaining.push_back(v);
    }
    std::sort(remaining.begin(), remaining.end());
    if (remaining.empty())
        return;

    int n = static_cast<int>(remaining.size());
    int slots = std::min(kNumStatsSlots, n);
    for (int i = 0; i < slots; i++) {
        int idx = (n * i) / slots;
        hist_values->push_back(remaining[idx]);
    }
}

// Serialize MCV (values + frequencies) as "v1:f1,v2:f2,...".
std::string SerializeMcvInt(const std::vector<int32_t>& values, const std::vector<float>& freqs) {
    std::string result;
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0)
            result += ',';
        result += std::to_string(values[i]) + ':' + std::to_string(freqs[i]);
    }
    return result;
}

std::string SerializeMcvText(const std::vector<std::string>& values,
                             const std::vector<float>& freqs) {
    std::string result;
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0)
            result += ',';
        result += values[i] + ':' + std::to_string(freqs[i]);
    }
    return result;
}

// Serialize histogram as "v1,v2,v3,...".
std::string SerializeHistInt(const std::vector<int32_t>& values) {
    std::string result;
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0)
            result += ',';
        result += std::to_string(values[i]);
    }
    return result;
}

std::string SerializeHistText(const std::vector<std::string>& values) {
    std::string result;
    for (size_t i = 0; i < values.size(); i++) {
        if (i > 0)
            result += ',';
        result += values[i];
    }
    return result;
}

// Analyze a single relation: scan heap, compute stats, write pg_statistic.
void AnalyzeRelation(Oid relid) {
    const FormData_pg_class* cls = GetCatalog()->GetClassByOid(relid);
    if (cls == nullptr)
        return;
    // Only analyze ordinary tables.
    if (cls->relkind != RelKind::kRelation)
        return;

    Relation rel = RelationOpen(relid);
    if (rel == nullptr)
        return;

    // Get attributes.
    auto attrs = GetCatalog()->GetAttributes(relid);
    int natts = 0;
    for (const auto* attr : attrs) {
        if (attr->attnum >= 1)
            natts++;
    }
    if (natts == 0) {
        RelationClose(rel);
        return;
    }

    // Scan all tuples and collect per-column samples.
    int total_rows = 0;
    std::vector<ColumnSamples> columns(natts + 1);  // 1-based indexing
    for (auto& col : columns) {
        col.int_values.clear();
        col.text_values.clear();
        col.null_count = 0;
        col.total_rows = 0;
        col.width_sum = 0;
    }

    HeapScanDesc scan = heap_beginscan(rel, nullptr);
    HeapTuple tuple = nullptr;
    while ((tuple = heap_getnext(scan)) != nullptr) {
        total_rows++;
        for (const auto* attr : attrs) {
            int attnum = attr->attnum;
            if (attnum < 1 || attnum > natts)
                continue;
            bool isnull = false;
            Datum d = heap_getattr(tuple, attnum, rel->rd_att, &isnull);
            auto& col = columns[attnum];
            col.total_rows++;
            if (isnull) {
                col.null_count++;
                continue;
            }
            if (attr->atttypid == kInt4Oid) {
                col.int_values.push_back(DatumGetInt32(d));
                col.width_sum += 4;
            } else if (attr->atttypid == kTextOid) {
                const char* text = pgcpp::types::DatumGetTextP(d);
                int len = pgcpp::types::VARSIZE_DATA(text);
                col.text_values.emplace_back(pgcpp::types::VARDATA(text), len);
                col.width_sum += len;
            } else {
                // Unknown type: just count width.
                col.width_sum += (attr->attlen > 0) ? attr->attlen : 8;
            }
        }
    }
    heap_endscan(scan);

    // Update pg_class.relpages and reltuples.
    int relpages = static_cast<int>(RelationGetNumberOfBlocks(rel));
    FormData_pg_class updated_class = *cls;
    updated_class.relpages = relpages;
    updated_class.reltuples = static_cast<float>(total_rows);
    GetCatalog()->UpdateClass(relid, &updated_class);

    // Delete old stats for this relation, then insert new ones.
    GetCatalog()->DeleteStatisticsForRelid(relid);

    // Compute and insert per-column statistics.
    for (const auto* attr : attrs) {
        int attnum = attr->attnum;
        if (attnum < 1 || attnum > natts)
            continue;
        const auto& col = columns[attnum];

        auto* stat = makePallocNode<FormData_pg_statistic>();
        stat->starelid = relid;
        stat->staattnum = static_cast<int16_t>(attnum);
        stat->stainherit = false;
        stat->stanullfrac = ComputeNullFraction(col.null_count, col.total_rows);
        stat->stawidth = ComputeAvgWidth(col.width_sum, col.total_rows);
        stat->stadistinct = 0;

        if (attr->atttypid == kInt4Oid && !col.int_values.empty()) {
            stat->stadistinct = ComputeDistinctInt(col.int_values);

            std::vector<int32_t> mcv_values;
            std::vector<float> mcv_freqs;
            ComputeMcvInt(col.int_values, kNumStatsSlots, &mcv_values, &mcv_freqs);
            stat->stakind1 = kStatisticKindMcv;
            stat->stavalues1 = SerializeMcvInt(mcv_values, mcv_freqs);

            std::vector<int32_t> hist_values;
            ComputeHistogramInt(col.int_values, mcv_values, &hist_values);
            stat->stakind2 = kStatisticKindHistogram;
            stat->stavalues2 = SerializeHistInt(hist_values);
        } else if (attr->atttypid == kTextOid && !col.text_values.empty()) {
            stat->stadistinct = ComputeDistinctText(col.text_values);

            std::vector<std::string> mcv_values;
            std::vector<float> mcv_freqs;
            ComputeMcvText(col.text_values, kNumStatsSlots, &mcv_values, &mcv_freqs);
            stat->stakind1 = kStatisticKindMcv;
            stat->stavalues1 = SerializeMcvText(mcv_values, mcv_freqs);

            std::vector<std::string> hist_values;
            ComputeHistogramText(col.text_values, mcv_values, &hist_values);
            stat->stakind2 = kStatisticKindHistogram;
            stat->stavalues2 = SerializeHistText(hist_values);
        }

        GetCatalog()->InsertStatistic(stat);
    }

    RelationClose(rel);
}

}  // namespace

std::string AnalyzeCommand(VacuumStmt* stmt) {
    if (stmt == nullptr)
        return "ANALYZE";

    if (stmt->rels.empty()) {
        // ANALYZE with no relations: analyze all user tables.
        auto all_classes = GetCatalog()->GetAllClasses();
        for (const auto* cls : all_classes) {
            if (cls->relkind == RelKind::kRelation &&
                cls->oid >= pgcpp::catalog::kFirstNormalObjectId)
                AnalyzeRelation(cls->oid);
        }
    } else {
        // Analyze specified relations.
        for (Node* node : stmt->rels) {
            if (node == nullptr)
                continue;
            auto* rv = static_cast<RangeVar*>(node);
            const FormData_pg_class* cls = GetCatalog()->GetClassByName(rv->relname);
            if (cls == nullptr) {
                ereport(pgcpp::error::LogLevel::kWarning,
                        "ANALYZE: relation \"" + rv->relname + "\" does not exist");
                continue;
            }
            AnalyzeRelation(cls->oid);
        }
    }

    return "ANALYZE";
}

}  // namespace pgcpp::commands
