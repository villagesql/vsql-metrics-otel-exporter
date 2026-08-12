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

#include <cstdlib>
#include <string>

#include "collector.h"

namespace vsql_metrics {

bool collect_binlog_size(CollectorCtx &ctx) {
  // SHOW BINARY LOGS raises an error outright when binary logging is off, so
  // the guard is a precondition rather than an optimisation.
  bool log_bin = false;
  if (!query_rows(ctx, "SELECT @@log_bin", [&](const sq::SqlQuery::Row &row) {
        log_bin = col_int(row, 0) != 0;
      })) {
    return false;
  }
  if (!log_bin) return true;

  long long total_bytes = 0;
  long long files = 0;
  long long last_file_number = 0;

  // SHOW BINARY LOGS, not SHOW MASTER LOGS -- the latter was removed in 8.4.
  const bool ok = query_rows(
      ctx, "SHOW BINARY LOGS", [&](const sq::SqlQuery::Row &row) {
        const std::string name(col_str(row, 0));
        total_bytes += col_int(row, 1);
        ++files;

        // Binlog names are "<basename>.<zero-padded sequence>"; the sequence of
        // the last row is the current file number.
        const std::size_t dot = name.rfind('.');
        if (dot != std::string::npos) {
          last_file_number = std::strtoll(name.c_str() + dot + 1, nullptr, 10);
        }
      });
  if (!ok) return false;

  ctx.out->gauge_i("mysql_binlog_size_bytes", total_bytes);
  ctx.out->gauge_i("mysql_binlog_files", files);
  ctx.out->gauge_i("mysql_binlog_file_number", last_file_number);
  return true;
}

}  // namespace vsql_metrics
