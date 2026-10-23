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
#include <string_view>

#include "collector.h"
#include "value_parse.h"

namespace vsql_metrics {
namespace {

// SHOW GLOBAL STATUS, not performance_schema.global_status. The
// performance_schema view omits every Com_* counter -- 169 of the 505 rows on a
// stock server -- which would silently delete mysql_global_status_commands_total.
constexpr const char kQuery[] = "SHOW GLOBAL STATUS";

// The prefixes upstream matches with a single alternation regexp. Each one maps
// its suffix straight onto a label of one counter; the buffer-pool case below
// is the only prefix that needs real per-suffix logic. The prefixes do not
// overlap, so the order of this table does not matter.
struct PrefixRule {
  std::string_view prefix;
  std::string_view metric;
  std::string_view label;
};

constexpr PrefixRule kPrefixRules[] = {
    {"com_", "mysql_global_status_commands_total", "command"},
    {"handler_", "mysql_global_status_handlers_total", "handler"},
    {"connection_errors_", "mysql_global_status_connection_errors_total", "error"},
    {"innodb_rows_", "mysql_global_status_innodb_row_ops_total", "operation"},
    {"performance_schema_", "mysql_global_status_performance_schema_lost_total",
     "instrumentation"},
};

// Routes one lowercased status key to its metric. Mirrors the switch in
// mysqld_exporter's global_status.go.
void emit(MetricBatch &out, const std::string &key, double value) {
  // VillageSQL's own status variables are reported by collect_villagesql,
  // which types the counters among them as sums and names them villagesql_*.
  // Letting them also fall through to the gauge default below would publish a
  // second series per counter under a mysql_ name that upstream never emits.
  if (!strip_prefix(key, "villagesql_").empty()) return;

  for (const PrefixRule &rule : kPrefixRules) {
    const std::string_view suffix = strip_prefix(key, rule.prefix);
    if (suffix.empty()) continue;
    out.counter_i(rule.metric, static_cast<long long>(value),
                  {{rule.label, std::string(suffix)}});
    return;
  }

  std::string_view suffix;
  if (!(suffix = strip_prefix(key, "innodb_buffer_pool_pages_")).empty()) {
    if (suffix == "data" || suffix == "free" || suffix == "misc" ||
        suffix == "old") {
      out.gauge_d("mysql_global_status_buffer_pool_pages", value,
                  {{"state", std::string(suffix)}});
    } else if (suffix == "dirty") {
      // Broken out of the state-labelled series on purpose: dirty pages are a
      // subset of the others, so including them would double-count under a
      // sum by (state).
      out.gauge_d("mysql_global_status_buffer_pool_dirty_pages", value);
    } else if (suffix == "total") {
      // Skipped upstream -- it is the sum of the other states.
    } else {
      out.counter_i("mysql_global_status_buffer_pool_page_changes_total",
                    static_cast<long long>(value),
                    {{"operation", std::string(suffix)}});
    }
    return;
  }

  // Upstream types the fallback as Untyped, which OTLP has no equivalent for.
  // Gauge is the closest and, more importantly, the metric name is identical
  // either way -- which is what dashboard parity depends on.
  out.gauge_d("mysql_global_status_" + key, value);
}

}  // namespace

bool collect_global_status(CollectorCtx &ctx) {
  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string key = valid_prometheus_name(col_str(row, 0));
    if (key.empty()) return;

    double value = 0.0;
    // Unparseable values yield no metric at all. This is what drops the PEM
    // blobs (Rsa_public_key, Caching_sha2_password_rsa_public_key) rather than
    // emitting them as a label or a zero.
    if (!parse_status_value(col_str(row, 1), &value)) return;

    emit(*ctx.out, key, value);
  });
}

}  // namespace vsql_metrics
