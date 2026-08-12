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
#include <string_view>

#include "collector.h"

namespace vsql_metrics {
namespace {

constexpr const char kQuery[] = "SHOW ENGINE INNODB STATUS";

// Reads a run of digits starting at `pos`, advancing it past them.
bool read_number(std::string_view text, std::size_t &pos, long long *out) {
  const std::size_t start = pos;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
  if (pos == start) return false;
  *out = std::strtoll(std::string(text.substr(start, pos - start)).c_str(),
                      nullptr, 10);
  return true;
}

// Matches "<n> queries inside InnoDB, <m> queries in queue", the shape of
// upstream's queriesRe. Written as a scan rather than a regex because it runs
// over a monitor blob that can reach a megabyte.
bool parse_queries_line(std::string_view line, long long *inside,
                        long long *queued) {
  std::size_t pos = 0;
  if (!read_number(line, pos, inside)) return false;

  constexpr std::string_view kMiddle = " queries inside InnoDB, ";
  if (line.compare(pos, kMiddle.size(), kMiddle) != 0) return false;
  pos += kMiddle.size();

  if (!read_number(line, pos, queued)) return false;
  return line.compare(pos, std::string_view::npos, " queries in queue") == 0;
}

// Matches "<n> read views open inside InnoDB".
bool parse_read_views_line(std::string_view line, long long *views) {
  std::size_t pos = 0;
  if (!read_number(line, pos, views)) return false;
  return line.compare(pos, std::string_view::npos,
                      " read views open inside InnoDB") == 0;
}

std::string_view trim_cr(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

}  // namespace

bool collect_engine_innodb_status(CollectorCtx &ctx) {
  std::string status;

  // The monitor text is column 2 and can be large. It must be copied out of the
  // row before the row is invalidated.
  bool wrong_shape = false;
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    if (row.num_columns() < 3) {
      wrong_shape = true;
      return;
    }
    status.assign(col_str(row, 2));
  });
  if (!ok) return false;
  if (wrong_shape) {
    // Without this the collector would report success having emitted nothing,
    // which is indistinguishable from a healthy server with no activity.
    *ctx.err = "SHOW ENGINE INNODB STATUS did not return a Status column";
    return false;
  }
  if (status.empty()) return true;

  const std::string_view text(status);
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t newline = text.find('\n', start);
    const std::string_view line = trim_cr(text.substr(
        start, newline == std::string_view::npos ? std::string_view::npos
                                                 : newline - start));

    long long inside = 0;
    long long queued = 0;
    long long views = 0;
    if (parse_queries_line(line, &inside, &queued)) {
      ctx.out->gauge_i("mysql_engine_innodb_queries_inside_innodb", inside);
      ctx.out->gauge_i("mysql_engine_innodb_queries_in_queue", queued);
    } else if (parse_read_views_line(line, &views)) {
      ctx.out->gauge_i("mysql_engine_innodb_read_views_open_inside_innodb", views);
    }

    if (newline == std::string_view::npos) break;
    start = newline + 1;
  }
  return true;
}

}  // namespace vsql_metrics
