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
#include "value_parse.h"

namespace vsql_metrics {
namespace {

constexpr const char kQuery[] = "SHOW GLOBAL VARIABLES";

}  // namespace

bool collect_global_variables(CollectorCtx &ctx) {
  // Three string variables carry no numeric value of their own but together
  // form the mysql_version_info label set, so they are stashed while streaming
  // and emitted once at the end.
  std::string version;
  std::string version_comment;
  std::string innodb_version;

  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string_view raw_key = col_str(row, 0);
    const std::string_view raw_value = col_str(row, 1);

    if (raw_key == "version") {
      version = std::string(raw_value);
    } else if (raw_key == "version_comment") {
      version_comment = std::string(raw_value);
    } else if (raw_key == "innodb_version") {
      innodb_version = std::string(raw_value);
    }

    const std::string key = valid_prometheus_name(raw_key);
    if (key.empty()) return;

    double value = 0.0;
    // Roughly half of the 637 variables on a stock server are paths, enums or
    // CSV lists with no numeric reading; those produce no metric.
    if (!parse_status_value(raw_value, &value)) return;

    ctx.out->gauge_d("mysql_global_variables_" + key, value);
  });
  if (!ok) return false;

  if (!version.empty()) {
    ctx.out->gauge_i("mysql_version_info", 1,
                     {{"innodb_version", innodb_version},
                      {"version", version},
                      {"version_comment", version_comment}});
  }
  return true;
}

}  // namespace vsql_metrics
