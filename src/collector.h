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

#ifndef VSQL_METRICS_COLLECTOR_H
#define VSQL_METRICS_COLLECTOR_H

#include <villagesql/preview/sql_query.h>

#include <string>
#include <string_view>
#include <utility>

#include "metric_batch.h"

namespace vsql_metrics {

namespace sq = vsql::preview_sql_query;

// performance_schema timer columns are picoseconds; every *_seconds metric
// derived from one divides by this.
constexpr double kPicoseconds = 1e12;

// Snapshot of the tunable sysvars, taken once per tick so a SET GLOBAL landing
// mid-scrape cannot change the shape of a query between collectors.
// Every field is assigned from its sysvar at the top of each tick; the values
// are declared once, in the sysvar table, and not restated here.
struct ScrapeConfig {
  long long processlist_min_time = 0;
  long long stmt_limit = 0;
  long long stmt_timelimit = 0;
  long long stmt_digest_text_limit = 0;
  std::string cores_dir;  // empty: villagesql_core_files is not emitted
};

struct CollectorCtx {
  const sq::Session *session;
  MetricBatch *out;
  const ScrapeConfig *cfg;
  std::string *err;  // set by the collector when it returns false
};

// A collector is a function rather than a declarative {sql, mapper} pair
// because the shapes genuinely differ: SHOW GLOBAL STATUS is one metric per
// row, INNODB_CMP is five metrics per row sharing a label, info_schema.tables
// runs a query per schema, and SHOW ENGINE INNODB STATUS is text parsing. A
// table would need an escape hatch for most of them.
struct Collector {
  const char *id;      // mysqld_exporter --collect.<id> name
  const char *sysvar;  // this extension's collect_<...> sysvar name
  bool default_on;     // mirrors mysqld_exporter's default for that flag
  bool (*scrape)(CollectorCtx &);
};

enum CollectorIdx {
  kGlobalStatus = 0,
  kGlobalVariables,
  kSlaveStatus,
  kInnodbCmp,
  kInnodbCmpmem,
  kInnodbMetrics,
  kProcesslist,
  kInfoSchemaTables,
  kEventsStatements,
  kEventsStatementsSum,
  kEventsWaits,
  kTableIoWaits,
  kIndexIoWaits,
  kFileEvents,
  kTableLocks,
  kEngineInnodbStatus,
  kBinlogSize,
  kProcess,
  kVillageSQL,
  kCoreFiles,
  kCollectorCount
};

// Column accessors that tolerate a short row. The SDK's accessors do not bounds
// check the index at all, and a NULL column is only distinguishable from an
// empty string by the returned string_view's data() being null.
inline std::string_view col_str(const sq::SqlQuery::Row &row, unsigned i) {
  if (i >= row.num_columns()) return {};
  const std::string_view value = row.column_str(i);
  return (value.data() == nullptr) ? std::string_view{} : value;
}

inline long long col_int(const sq::SqlQuery::Row &row, unsigned i) {
  return (i < row.num_columns()) ? row.column_int(i) : 0;
}

inline double col_real(const sq::SqlQuery::Row &row, unsigned i) {
  return (i < row.num_columns()) ? row.column_real(i) : 0.0;
}

// Runs `sql`, invoking fn once per row. Returns false and fills ctx.err when
// the statement itself failed, which the driver turns into a per-collector
// error without aborting the rest of the scrape.
//
// The callback is wrapped because the SDK converts it to a C function pointer
// and the server calls it from a frame declared `extern "C"`. Collector
// callbacks allocate freely -- strings, vectors, and in one case a multi-
// megabyte blob copy -- so a std::bad_alloc escaping here would unwind across
// the ABI boundary and leave the server unable to close the result handle.
// Catching per row rather than per query also means one bad row does not
// discard the rows already collected.
template <typename F>
bool query_rows(CollectorCtx &ctx, const std::string &sql, F &&fn) {
  bool callback_threw = false;
  auto result = ctx.session->sql(sql).for_each(
      [&fn, &callback_threw](const sq::SqlQuery::Row &row) noexcept {
        try {
          fn(row);
        } catch (...) {
          callback_threw = true;
        }
      });

  if (result.has_error()) {
    const sq::Diag diag = result.error();
    *ctx.err = std::string(diag.message);
    if (ctx.err->empty()) {
      *ctx.err = "query failed with errno " + std::to_string(diag.errno_);
    }
    return false;
  }
  if (callback_threw) {
    *ctx.err = "row handler threw while processing results";
    return false;
  }
  return true;
}

// Collector implementations, one group per source file under collectors/.
bool collect_global_status(CollectorCtx &ctx);
bool collect_global_variables(CollectorCtx &ctx);
bool collect_slave_status(CollectorCtx &ctx);
bool collect_innodb_cmp(CollectorCtx &ctx);
bool collect_innodb_cmpmem(CollectorCtx &ctx);
bool collect_innodb_metrics(CollectorCtx &ctx);
bool collect_processlist(CollectorCtx &ctx);
bool collect_info_schema_tables(CollectorCtx &ctx);
bool collect_events_statements(CollectorCtx &ctx);
bool collect_events_statements_sum(CollectorCtx &ctx);
bool collect_events_waits(CollectorCtx &ctx);
bool collect_table_io_waits(CollectorCtx &ctx);
bool collect_index_io_waits(CollectorCtx &ctx);
bool collect_file_events(CollectorCtx &ctx);
bool collect_table_locks(CollectorCtx &ctx);
bool collect_engine_innodb_status(CollectorCtx &ctx);
bool collect_binlog_size(CollectorCtx &ctx);
bool collect_process(CollectorCtx &ctx);
bool collect_villagesql(CollectorCtx &ctx);
bool collect_core_files(CollectorCtx &ctx);

}  // namespace vsql_metrics

#endif  // VSQL_METRICS_COLLECTOR_H
