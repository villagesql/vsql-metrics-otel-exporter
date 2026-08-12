/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <string>
#include <vector>

#include "collector.h"

namespace vsql_metrics {
namespace {

// The four I/O operations share a column layout in both table_io_waits tables:
// four COUNT_* columns followed by four matching SUM_TIMER_* columns.
struct IoOperation {
  std::string_view name;
  unsigned count_idx;
  unsigned timer_idx;
  // Inserts are not attributable to a particular index, so upstream reports
  // them only on the synthetic NONE row. Carried as a flag rather than
  // rediscovered by comparing the label text.
  bool none_index_only = false;
};

// Refuses to emit when the result does not have the column count the index
// constants below were written against. Without this a column added to one of
// these SELECTs would shift every operation onto its neighbour's column and be
// reported indefinitely as a successful scrape of wrong numbers.
bool layout_ok(const sq::SqlQuery::Row &row, unsigned expected,
               CollectorCtx &ctx, const char *table) {
  if (row.num_columns() == expected) return true;
  *ctx.err = std::string(table) + " returned " +
             std::to_string(row.num_columns()) + " columns, expected " +
             std::to_string(expected);
  return false;
}

}  // namespace

bool collect_table_io_waits(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT OBJECT_SCHEMA, OBJECT_NAME, COUNT_FETCH, COUNT_INSERT, "
      "COUNT_UPDATE, COUNT_DELETE, SUM_TIMER_FETCH, SUM_TIMER_INSERT, "
      "SUM_TIMER_UPDATE, SUM_TIMER_DELETE "
      "FROM performance_schema.table_io_waits_summary_by_table "
      "WHERE OBJECT_SCHEMA NOT IN ('mysql', 'performance_schema')";

  static constexpr IoOperation kOperations[] = {
      {"fetch", 2, 6}, {"insert", 3, 7}, {"update", 4, 8}, {"delete", 5, 9}};
  constexpr unsigned kExpectedColumns = 10;

  bool layout_failed = false;
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    if (layout_failed) return;
    if (!layout_ok(row, kExpectedColumns, ctx, "table_io_waits_summary_by_table")) {
      layout_failed = true;
      return;
    }
    const std::string schema(col_str(row, 0));
    const std::string name(col_str(row, 1));

    for (const IoOperation &op : kOperations) {
      // Upstream labels the table "name", not "table".
      const std::vector<Label> labels = {
          {"schema", schema}, {"name", name}, {"operation", std::string(op.name)}};
      ctx.out->counter_i("mysql_perf_schema_table_io_waits_total",
                         col_int(row, op.count_idx), labels);
      ctx.out->counter_d("mysql_perf_schema_table_io_waits_seconds_total",
                         col_real(row, op.timer_idx) / kPicoseconds, labels);
    }
  });
  return ok && !layout_failed;
}

bool collect_index_io_waits(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT OBJECT_SCHEMA, OBJECT_NAME, ifnull(INDEX_NAME, 'NONE') AS "
      "INDEX_NAME, COUNT_FETCH, COUNT_INSERT, COUNT_UPDATE, COUNT_DELETE, "
      "SUM_TIMER_FETCH, SUM_TIMER_INSERT, SUM_TIMER_UPDATE, SUM_TIMER_DELETE "
      "FROM performance_schema.table_io_waits_summary_by_index_usage "
      "WHERE OBJECT_SCHEMA NOT IN ('mysql', 'performance_schema')";

  static constexpr IoOperation kOperations[] = {{"fetch", 3, 7},
                                                {"insert", 4, 8, true},
                                                {"update", 5, 9},
                                                {"delete", 6, 10}};
  constexpr unsigned kExpectedColumns = 11;

  bool layout_failed = false;
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    if (layout_failed) return;
    if (!layout_ok(row, kExpectedColumns, ctx,
                   "table_io_waits_summary_by_index_usage")) {
      layout_failed = true;
      return;
    }
    const std::string schema(col_str(row, 0));
    const std::string name(col_str(row, 1));
    const std::string index(col_str(row, 2));

    for (const IoOperation &op : kOperations) {
      if (op.none_index_only && index != "NONE") continue;

      const std::vector<Label> labels = {{"schema", schema},
                                         {"name", name},
                                         {"index", index},
                                         {"operation", std::string(op.name)}};
      ctx.out->counter_i("mysql_perf_schema_index_io_waits_total",
                         col_int(row, op.count_idx), labels);
      ctx.out->counter_d("mysql_perf_schema_index_io_waits_seconds_total",
                         col_real(row, op.timer_idx) / kPicoseconds, labels);
    }
  });
  return ok && !layout_failed;
}

bool collect_table_locks(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT OBJECT_SCHEMA, OBJECT_NAME, COUNT_READ_NORMAL, "
      "COUNT_READ_WITH_SHARED_LOCKS, COUNT_READ_HIGH_PRIORITY, "
      "COUNT_READ_NO_INSERT, COUNT_READ_EXTERNAL, COUNT_WRITE_ALLOW_WRITE, "
      "COUNT_WRITE_CONCURRENT_INSERT, COUNT_WRITE_LOW_PRIORITY, "
      "COUNT_WRITE_NORMAL, COUNT_WRITE_EXTERNAL, SUM_TIMER_READ_NORMAL, "
      "SUM_TIMER_READ_WITH_SHARED_LOCKS, SUM_TIMER_READ_HIGH_PRIORITY, "
      "SUM_TIMER_READ_NO_INSERT, SUM_TIMER_READ_EXTERNAL, "
      "SUM_TIMER_WRITE_ALLOW_WRITE, SUM_TIMER_WRITE_CONCURRENT_INSERT, "
      "SUM_TIMER_WRITE_LOW_PRIORITY, SUM_TIMER_WRITE_NORMAL, "
      "SUM_TIMER_WRITE_EXTERNAL "
      "FROM performance_schema.table_lock_waits_summary_by_table "
      "WHERE OBJECT_SCHEMA NOT IN "
      "('mysql', 'performance_schema', 'information_schema')";

  // The EXTERNAL columns are reported under a separate metric from the rest,
  // which is why the two groups are listed apart.
  struct LockOperation {
    std::string_view name;
    unsigned count_idx;
    unsigned timer_idx;
    bool external;
  };
  static constexpr LockOperation kOperations[] = {
      {"read_normal", 2, 12, false},
      {"read_with_shared_locks", 3, 13, false},
      {"read_high_priority", 4, 14, false},
      {"read_no_insert", 5, 15, false},
      {"read", 6, 16, true},
      {"write_allow_write", 7, 17, false},
      {"write_concurrent_insert", 8, 18, false},
      {"write_low_priority", 9, 19, false},
      {"write_normal", 10, 20, false},
      {"write", 11, 21, true}};

  constexpr unsigned kExpectedColumns = 22;

  bool layout_failed = false;
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    if (layout_failed) return;
    if (!layout_ok(row, kExpectedColumns, ctx,
                   "table_lock_waits_summary_by_table")) {
      layout_failed = true;
      return;
    }
    const std::string schema(col_str(row, 0));
    const std::string name(col_str(row, 1));

    for (const LockOperation &op : kOperations) {
      const std::vector<Label> labels = {
          {"schema", schema}, {"name", name}, {"operation", std::string(op.name)}};
      const std::string_view count_metric =
          op.external ? "mysql_perf_schema_external_lock_waits_total"
                      : "mysql_perf_schema_sql_lock_waits_total";
      const std::string_view seconds_metric =
          op.external ? "mysql_perf_schema_external_lock_waits_seconds_total"
                      : "mysql_perf_schema_sql_lock_waits_seconds_total";

      ctx.out->counter_i(count_metric, col_int(row, op.count_idx), labels);
      ctx.out->counter_d(seconds_metric,
                         col_real(row, op.timer_idx) / kPicoseconds, labels);
    }
  });
  return ok && !layout_failed;
}

}  // namespace vsql_metrics
