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

#include "collector.h"

namespace vsql_metrics {
namespace {

// Extension worker threads are registered with performance_schema under this
// instrumented name; a steadily growing count is a thread leak in some
// extension. This exporter's own worker is one of them.
constexpr const char kWorkerThreadsQuery[] =
    "SELECT COUNT(*) FROM performance_schema.threads "
    "WHERE name LIKE '%vef_extension_worker%'";

// One row per installed extension. PENDING_LAST_ERROR is set when an
// install/upgrade got stuck or failed; any non-null value needs a human.
constexpr const char kExtensionsQuery[] =
    "SELECT extension_name, extension_version, "
    "pending_last_error IS NOT NULL "
    "FROM information_schema.extensions";

}  // namespace

// VillageSQL extension-framework state, which no mysqld_exporter collector can
// see. These have no upstream equivalent, so the names carry a villagesql_
// prefix rather than mysql_.
bool collect_villagesql(CollectorCtx &ctx) {
  bool ok = query_rows(ctx, kWorkerThreadsQuery, [&](const sq::SqlQuery::Row &row) {
    ctx.out->gauge_i("villagesql_vef_worker_threads", col_int(row, 0));
  });
  if (!ok) return false;

  long long pending_errors = 0;
  ok = query_rows(ctx, kExtensionsQuery, [&](const sq::SqlQuery::Row &row) {
    ctx.out->gauge_i("villagesql_extension_info", 1,
                     {{"extension_name", std::string(col_str(row, 0))},
                      {"extension_version", std::string(col_str(row, 1))}});
    pending_errors += col_int(row, 2);
  });
  if (!ok) return false;

  ctx.out->gauge_i("villagesql_extension_pending_errors", pending_errors);
  return true;
}

}  // namespace vsql_metrics
