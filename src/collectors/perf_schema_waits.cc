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

bool collect_events_waits(CollectorCtx &ctx) {
  // The summary table carries a row for every wait instrument the server knows
  // about, enabled or not -- 405 on a stock server, of which only 54 are
  // enabled. Joining setup_instruments restricts the export to instruments that
  // can actually accumulate.
  //
  // The join runs on every scrape, so the enabled set is resolved fresh each
  // time: an instrument enabled by UPDATE setup_instruments starts being
  // exported on the very next tick with no restart and no configuration change
  // here. Nothing about the instrument list is cached anywhere.
  //
  // Deliberately NOT filtered on i.TIMED. ENABLED='YES' with TIMED='NO' is a
  // real state -- COUNT_STAR still increments while SUM_TIMER_WAIT stays 0 --
  // so a TIMED filter would drop instruments that are actively firing.
  //
  // The idle exclusion is kept from the previous query. 'idle' is an enabled
  // instrument, so the join alone would start emitting it, and mysqld_exporter
  // excludes it -- adding it would break the name-for-name parity this exporter
  // exists to provide.
  constexpr const char kQuery[] =
      "SELECT e.EVENT_NAME, e.COUNT_STAR, e.SUM_TIMER_WAIT "
      "FROM performance_schema.events_waits_summary_global_by_event_name e "
      "JOIN performance_schema.setup_instruments i ON i.NAME = e.EVENT_NAME "
      "WHERE i.ENABLED = 'YES' AND e.EVENT_NAME != 'idle'";

  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::vector<Label> labels = {
        {"event_name", std::string(col_str(row, 0))}};

    ctx.out->counter_i("mysql_perf_schema_events_waits_total", col_int(row, 1),
                       labels);
    ctx.out->counter_d("mysql_perf_schema_events_waits_seconds_total",
                       col_real(row, 2) / kPicoseconds, labels);
  });
}

bool collect_file_events(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT EVENT_NAME, COUNT_READ, SUM_TIMER_READ, SUM_NUMBER_OF_BYTES_READ, "
      "COUNT_WRITE, SUM_TIMER_WRITE, SUM_NUMBER_OF_BYTES_WRITE, "
      "COUNT_MISC, SUM_TIMER_MISC "
      "FROM performance_schema.file_summary_by_event_name";

  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string event_name(col_str(row, 0));

    auto emit = [&](const char *mode, unsigned count_idx, unsigned timer_idx) {
      const std::vector<Label> labels = {{"event_name", event_name},
                                         {"mode", mode}};
      ctx.out->counter_i("mysql_perf_schema_file_events_total",
                         col_int(row, count_idx), labels);
      ctx.out->counter_d("mysql_perf_schema_file_events_seconds_total",
                         col_real(row, timer_idx) / kPicoseconds, labels);
    };

    emit("read", 1, 2);
    emit("write", 4, 5);
    // The misc mode has no byte counter in the source table, which is why
    // file_events_bytes_total carries only read and write.
    emit("misc", 7, 8);

    ctx.out->counter_i("mysql_perf_schema_file_events_bytes_total",
                       col_int(row, 3),
                       {{"event_name", event_name}, {"mode", "read"}});
    ctx.out->counter_i("mysql_perf_schema_file_events_bytes_total",
                       col_int(row, 6),
                       {{"event_name", event_name}, {"mode", "write"}});
  });
}

}  // namespace vsql_metrics
