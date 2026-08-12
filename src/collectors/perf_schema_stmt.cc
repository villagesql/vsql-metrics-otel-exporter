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

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "collector.h"

namespace vsql_metrics {

bool collect_events_statements(CollectorCtx &ctx) {
  const std::string query =
      "SELECT ifnull(SCHEMA_NAME, 'NONE') AS SCHEMA_NAME, DIGEST, "
      "LEFT(DIGEST_TEXT, " +
      std::to_string(ctx.cfg->stmt_digest_text_limit) +
      ") AS DIGEST_TEXT, COUNT_STAR, SUM_TIMER_WAIT, SUM_ERRORS, SUM_WARNINGS, "
      "SUM_ROWS_AFFECTED, SUM_ROWS_SENT, SUM_ROWS_EXAMINED, "
      "SUM_CREATED_TMP_DISK_TABLES, SUM_CREATED_TMP_TABLES, "
      "SUM_SORT_MERGE_PASSES, SUM_SORT_ROWS, SUM_NO_INDEX_USED, SUM_LOCK_TIME "
      "FROM performance_schema.events_statements_summary_by_digest "
      "WHERE DIGEST_TEXT IS NOT NULL AND LAST_SEEN > DATE_SUB(NOW(), INTERVAL " +
      std::to_string(ctx.cfg->stmt_timelimit) +
      " SECOND) ORDER BY SUM_TIMER_WAIT DESC LIMIT " +
      std::to_string(ctx.cfg->stmt_limit);

  return query_rows(ctx, query, [&](const sq::SqlQuery::Row &row) {
    const std::vector<Label> labels = {
        {"schema", std::string(col_str(row, 0))},
        {"digest", std::string(col_str(row, 1))},
        {"digest_text", std::string(col_str(row, 2))}};

    ctx.out->counter_i("mysql_perf_schema_events_statements_total",
                       col_int(row, 3), labels);
    ctx.out->counter_d("mysql_perf_schema_events_statements_seconds_total",
                       col_real(row, 4) / kPicoseconds, labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_errors_total",
                       col_int(row, 5), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_warnings_total",
                       col_int(row, 6), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_rows_affected_total",
                       col_int(row, 7), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_rows_sent_total",
                       col_int(row, 8), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_rows_examined_total",
                       col_int(row, 9), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_tmp_disk_tables_total",
                       col_int(row, 10), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_tmp_tables_total",
                       col_int(row, 11), labels);
    ctx.out->counter_i(
        "mysql_perf_schema_events_statements_sort_merge_passes_total",
        col_int(row, 12), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_sort_rows_total",
                       col_int(row, 13), labels);
    ctx.out->counter_i("mysql_perf_schema_events_statements_no_index_used_total",
                       col_int(row, 14), labels);
    ctx.out->counter_d("mysql_perf_schema_events_statements_lock_time_seconds_total",
                       col_real(row, 15) / kPicoseconds, labels);
  });
}

bool collect_events_statements_sum(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT SUM(COUNT_STAR), SUM(SUM_CREATED_TMP_DISK_TABLES), "
      "SUM(SUM_CREATED_TMP_TABLES), SUM(SUM_ERRORS), SUM(SUM_LOCK_TIME), "
      "SUM(SUM_NO_GOOD_INDEX_USED), SUM(SUM_NO_INDEX_USED), "
      "SUM(SUM_ROWS_AFFECTED), SUM(SUM_ROWS_EXAMINED), SUM(SUM_ROWS_SENT), "
      "SUM(SUM_SELECT_FULL_JOIN), SUM(SUM_SELECT_FULL_RANGE_JOIN), "
      "SUM(SUM_SELECT_RANGE), SUM(SUM_SELECT_RANGE_CHECK), SUM(SUM_SELECT_SCAN), "
      "SUM(SUM_SORT_MERGE_PASSES), SUM(SUM_SORT_RANGE), SUM(SUM_SORT_ROWS), "
      "SUM(SUM_SORT_SCAN), SUM(SUM_TIMER_WAIT), SUM(SUM_WARNINGS) "
      "FROM performance_schema.events_statements_summary_by_digest";

  // One entry per SELECT column, in the same order. Note the metric names carry
  // no _total suffix except the first, and that only timer_wait is converted
  // from picoseconds -- lock_time is a duration too but upstream leaves it raw.
  // Both are upstream's spelling, kept for parity. The conversion travels with
  // the entry so inserting a column cannot move it onto the wrong metric.
  struct SumMetric {
    std::string_view suffix;
    bool picoseconds;
  };
  static constexpr SumMetric kMetrics[] = {
      {"total", false},
      {"created_tmp_disk_tables", false},
      {"created_tmp_tables", false},
      {"errors", false},
      {"lock_time", false},
      {"no_good_index_used", false},
      {"no_index_used", false},
      {"rows_affected", false},
      {"rows_examined", false},
      {"rows_sent", false},
      {"select_full_join", false},
      {"select_full_range_join", false},
      {"select_range", false},
      {"select_range_check", false},
      {"select_scan", false},
      {"sort_merge_passes", false},
      {"sort_range", false},
      {"sort_rows", false},
      {"sort_scan", false},
      {"timer_wait", true},
      {"warnings", false}};
  constexpr unsigned kMetricCount = static_cast<unsigned>(std::size(kMetrics));

  bool layout_failed = false;
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    if (row.num_columns() != kMetricCount) {
      *ctx.err = "events_statements_summary_by_digest aggregate returned " +
                 std::to_string(row.num_columns()) + " columns, expected " +
                 std::to_string(kMetricCount);
      layout_failed = true;
      return;
    }
    for (unsigned i = 0; i < kMetricCount; ++i) {
      const std::string metric =
          std::string("mysql_perf_schema_events_statements_sum_") +
          std::string(kMetrics[i].suffix);
      if (kMetrics[i].picoseconds) {
        ctx.out->counter_d(metric, col_real(row, i) / kPicoseconds);
      } else {
        ctx.out->counter_i(metric, col_int(row, i));
      }
    }
  });
  return ok && !layout_failed;
}

}  // namespace vsql_metrics
