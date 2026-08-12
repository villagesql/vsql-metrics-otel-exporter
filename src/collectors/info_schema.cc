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

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "collector.h"

namespace vsql_metrics {

bool collect_innodb_cmp(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT page_size, compress_ops, compress_ops_ok, compress_time, "
      "uncompress_ops, uncompress_time FROM information_schema.innodb_cmp";

  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::vector<Label> labels = {
        {"page_size", std::string(col_str(row, 0))}};

    ctx.out->counter_i("mysql_info_schema_innodb_cmp_compress_ops_total",
                       col_int(row, 1), labels);
    ctx.out->counter_i("mysql_info_schema_innodb_cmp_compress_ops_ok_total",
                       col_int(row, 2), labels);
    // compress_time and uncompress_time are already seconds in this table.
    ctx.out->counter_d("mysql_info_schema_innodb_cmp_compress_time_seconds_total",
                       col_real(row, 3), labels);
    ctx.out->counter_i("mysql_info_schema_innodb_cmp_uncompress_ops_total",
                       col_int(row, 4), labels);
    ctx.out->counter_d(
        "mysql_info_schema_innodb_cmp_uncompress_time_seconds_total",
        col_real(row, 5), labels);
  });
}

bool collect_innodb_cmpmem(CollectorCtx &ctx) {
  constexpr const char kQuery[] =
      "SELECT page_size, buffer_pool_instance, pages_used, pages_free, "
      "relocation_ops, relocation_time FROM information_schema.innodb_cmpmem";

  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::vector<Label> labels = {
        {"page_size", std::string(col_str(row, 0))},
        {"buffer_pool", std::string(col_str(row, 1))}};

    // pages_used and pages_free are instantaneous counts, but upstream types
    // them as counters and names them _total. Kept as-is: changing either would
    // break the dashboards this exporter exists to serve.
    ctx.out->counter_i("mysql_info_schema_innodb_cmpmem_pages_used_total",
                       col_int(row, 2), labels);
    ctx.out->counter_i("mysql_info_schema_innodb_cmpmem_pages_free_total",
                       col_int(row, 3), labels);
    ctx.out->counter_i("mysql_info_schema_innodb_cmpmem_relocation_ops_total",
                       col_int(row, 4), labels);
    // relocation_time is milliseconds here, unlike innodb_cmp's seconds.
    ctx.out->counter_d(
        "mysql_info_schema_innodb_cmpmem_relocation_time_seconds_total",
        col_real(row, 5) / 1000.0, labels);
  });
}

bool collect_processlist(CollectorCtx &ctx) {
  const std::string query =
      "SELECT user, SUBSTRING_INDEX(host, ':', 1) AS host, "
      "COALESCE(command, '') AS command, COALESCE(state, '') AS state, "
      "COUNT(*) AS processes, SUM(time) AS seconds "
      "FROM information_schema.processlist "
      "WHERE ID != connection_id() AND TIME >= " +
      std::to_string(ctx.cfg->processlist_min_time) +
      " GROUP BY user, host, command, state";

  // The query groups by four columns but the metrics aggregate over different
  // subsets of them, so the rows are folded here rather than in four queries.
  struct ThreadTotals {
    long long threads = 0;
    long long seconds = 0;
  };
  std::map<std::pair<std::string, std::string>, ThreadTotals> by_command_state;
  std::map<std::string, long long> by_user;
  std::map<std::string, long long> by_host;

  const bool ok = query_rows(ctx, query, [&](const sq::SqlQuery::Row &row) {
    const std::string user(col_str(row, 0));
    const std::string host(col_str(row, 1));
    const std::string command(col_str(row, 2));
    const std::string state(col_str(row, 3));
    const long long processes = col_int(row, 4);
    const long long seconds = col_int(row, 5);

    ThreadTotals &entry = by_command_state[{command, state}];
    entry.threads += processes;
    entry.seconds += seconds;
    by_user[user] += processes;
    by_host[host] += processes;
  });
  if (!ok) return false;

  for (const auto &entry : by_command_state) {
    const std::vector<Label> labels = {{"command", entry.first.first},
                                       {"state", entry.first.second}};
    ctx.out->gauge_i("mysql_info_schema_processlist_threads", entry.second.threads,
                     labels);
    ctx.out->gauge_i("mysql_info_schema_processlist_seconds", entry.second.seconds,
                     labels);
  }
  for (const auto &entry : by_user) {
    ctx.out->gauge_i("mysql_info_schema_processlist_processes_by_user",
                     entry.second, {{"mysql_user", entry.first}});
  }
  for (const auto &entry : by_host) {
    ctx.out->gauge_i("mysql_info_schema_processlist_processes_by_host",
                     entry.second, {{"client_host", entry.first}});
  }
  return true;
}

bool collect_info_schema_tables(CollectorCtx &ctx) {
  // mysqld_exporter lists the schemas first and then queries each one in turn.
  // The predicate is a plain equality on a column already being selected, so
  // one query with the same exclusion list returns exactly the same rows. Doing
  // it that way avoids interpolating a server-supplied schema name into SQL --
  // hand-escaping it would be wrong under NO_BACKSLASH_ESCAPES -- and drops an
  // N+1 against a data-dictionary view whose size columns are not free.
  constexpr const char kQuery[] =
      "SELECT TABLE_SCHEMA, TABLE_NAME, TABLE_TYPE, "
      "ifnull(ENGINE, 'NONE') AS ENGINE, ifnull(VERSION, '0') AS VERSION, "
      "ifnull(ROW_FORMAT, 'NONE') AS ROW_FORMAT, "
      "ifnull(TABLE_ROWS, '0') AS TABLE_ROWS, "
      "ifnull(DATA_LENGTH, '0') AS DATA_LENGTH, "
      "ifnull(INDEX_LENGTH, '0') AS INDEX_LENGTH, "
      "ifnull(DATA_FREE, '0') AS DATA_FREE, "
      "ifnull(CREATE_OPTIONS, 'NONE') AS CREATE_OPTIONS "
      "FROM information_schema.tables WHERE TABLE_SCHEMA NOT IN "
      "('mysql', 'performance_schema', 'information_schema', 'sys')";

  struct SizeComponent {
    std::string_view name;
    unsigned column;
  };
  static constexpr SizeComponent kComponents[] = {
      {"data_length", 7}, {"index_length", 8}, {"data_free", 9}};

  return query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    const std::string table_schema(col_str(row, 0));
    const std::string table_name(col_str(row, 1));

    ctx.out->gauge_i("mysql_info_schema_table_version", col_int(row, 4),
                     {{"schema", table_schema},
                      {"table", table_name},
                      {"type", std::string(col_str(row, 2))},
                      {"engine", std::string(col_str(row, 3))},
                      {"row_format", std::string(col_str(row, 5))},
                      {"create_options", std::string(col_str(row, 10))}});

    ctx.out->gauge_i("mysql_info_schema_table_rows", col_int(row, 6),
                     {{"schema", table_schema}, {"table", table_name}});

    for (const SizeComponent &component : kComponents) {
      ctx.out->gauge_i("mysql_info_schema_table_size", col_int(row, component.column),
                       {{"schema", table_schema},
                        {"table", table_name},
                        {"component", std::string(component.name)}});
    }
  });
}

}  // namespace vsql_metrics
