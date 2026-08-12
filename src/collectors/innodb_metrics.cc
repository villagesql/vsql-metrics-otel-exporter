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

constexpr const char kQuery[] =
    "SELECT name, subsystem, type, comment, count "
    "FROM information_schema.innodb_metrics WHERE status = 'enabled'";

// Upstream's ^buffer_page_(read|written)_(.*)$ over the buffer_page_io subsystem.
// Returns false when the name does not fit the pattern, which upstream warns
// about and skips.
bool route_buffer_page_io(MetricBatch &out, std::string_view name, double value) {
  std::string_view rest;
  if (!(rest = strip_prefix(name, "buffer_page_read_")).empty()) {
    out.counter_d("mysql_info_schema_innodb_metrics_buffer_page_read_total", value,
                  {{"type", std::string(rest)}});
    return true;
  }
  if (!(rest = strip_prefix(name, "buffer_page_written_")).empty()) {
    out.counter_d("mysql_info_schema_innodb_metrics_buffer_page_written_total",
                  value, {{"type", std::string(rest)}});
    return true;
  }
  return false;
}

// Upstream's ^buffer_(pool_pages)_(.*)$ over the buffer subsystem. Names that do
// not match fall through to the generic path, so this reports whether it
// handled the row.
bool route_buffer(MetricBatch &out, std::string_view name, double value) {
  const std::string_view state = strip_prefix(name, "buffer_pool_pages_");
  if (state.empty()) return false;

  if (state == "total") {
    // Ignored: an aggregation of the other states.
  } else if (state == "dirty") {
    // Not part of the state-labelled series -- dirty pages overlap the others.
    out.gauge_d("mysql_info_schema_innodb_metrics_buffer_pool_dirty_pages", value);
  } else {
    out.gauge_d("mysql_info_schema_innodb_metrics_buffer_pool_pages", value,
                {{"state", std::string(state)}});
  }
  return true;
}

}  // namespace

bool collect_innodb_metrics(CollectorCtx &ctx) {
  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string_view name = col_str(row, 0);
    const std::string_view subsystem = col_str(row, 1);
    const std::string_view type = col_str(row, 2);
    const double value = col_real(row, 4);

    if (subsystem == "buffer_page_io") {
      route_buffer_page_io(*ctx.out, name, value);
      return;
    }
    if (subsystem == "buffer" && route_buffer(*ctx.out, name, value)) return;

    const std::string metric = "mysql_info_schema_innodb_metrics_" +
                               valid_prometheus_name(subsystem) + "_" +
                               valid_prometheus_name(name);

    // MySQL spells counters two ways. The value >= 0 guard is upstream's, and
    // exists because some counters are reported negative (MySQL bug 75966); a
    // negative counter is emitted as a gauge instead.
    if ((type == "counter" || type == "status_counter") && value >= 0) {
      ctx.out->counter_d(metric + "_total", value);
    } else {
      ctx.out->gauge_d(metric, value);
    }
  });
}

}  // namespace vsql_metrics
