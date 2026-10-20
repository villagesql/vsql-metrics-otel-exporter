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

// Extension worker threads are registered with performance_schema under this
// instrumented name; a steadily growing count is a thread leak in some
// extension. This exporter's own worker is one of them.
constexpr const char kWorkerThreadsQuery[] =
    "SELECT COUNT(*) FROM performance_schema.threads "
    "WHERE name LIKE '%vef_extension_worker%'";

constexpr const char kPendingErrorsQuery[] =
    "SELECT COUNT(*) FROM information_schema.extensions "
    "WHERE pending_last_error IS NOT NULL";

// Every status variable under the Villagesql_ prefix. Matching the prefix
// rather than a list of names means a gauge added server-side is picked up
// with no change here; a counter still needs a line in kCounters below. The
// underscore is a LIKE wildcard, hence the escape.
constexpr const char kStatusVarQuery[] =
    "SHOW GLOBAL STATUS LIKE 'Villagesql\\_%'";

// Monotonic counters that take Kind::kSum and the _total suffix upstream gives
// counters. The list is opt-in because a gauge wrongly typed as a sum is the
// worse error: every decrease then reads downstream as a counter reset,
// whereas a counter left as a gauge only loses rate semantics.
//
// Entries are the sanitized name, so the server's Villagesql_veb_cache_repaired
// is matched as villagesql_veb_cache_repaired and emitted with _total.
constexpr std::string_view kCounters[] = {
    "villagesql_veb_cache_repaired",
};

}  // namespace

// VillageSQL's own metrics, which no mysqld_exporter collector can see: VEF
// framework state, plus the global status variables the server registers
// under the Villagesql_ prefix. These have no upstream equivalent, so the
// names carry a villagesql_ prefix rather than mysql_.
bool collect_villagesql(CollectorCtx &ctx) {
  bool ok = query_rows(ctx, kWorkerThreadsQuery, [&](const sq::SqlQuery::Row &row) {
    ctx.out->gauge_i("villagesql_vef_worker_threads", col_int(row, 0));
  });
  if (!ok) return false;

  ok = query_rows(ctx, kPendingErrorsQuery, [&](const sq::SqlQuery::Row &row) {
    ctx.out->gauge_i("villagesql_extension_pending_errors", col_int(row, 0));
  });
  if (!ok) return false;

  // A server without these variables returns no rows and emits nothing, which
  // is what keeps this collector working against one that predates them.
  return query_rows(ctx, kStatusVarQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string name = valid_prometheus_name(col_str(row, 0));
    if (name.empty()) return;

    double value = 0.0;
    if (!parse_status_value(col_str(row, 1), &value)) return;

    for (const std::string_view counter : kCounters) {
      if (name == counter) {
        ctx.out->counter_i(name + "_total", static_cast<long long>(value));
        return;
      }
    }
    ctx.out->gauge_d(name, value);
  });
}

}  // namespace vsql_metrics
