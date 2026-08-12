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

#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "collector.h"
#include "value_parse.h"

namespace vsql_metrics {
namespace {

// mysqld_exporter tries SHOW ALL SLAVES STATUS, then SHOW SLAVE STATUS, then
// SHOW REPLICA STATUS. MySQL 8.4 removed the first two outright -- they raise a
// syntax error rather than returning nothing -- so only the surviving spelling
// is issued. Metric names keep upstream's "slave_status" wording, since
// dashboard parity is the point.
constexpr const char kQuery[] = "SHOW REPLICA STATUS";

// Upstream builds each metric name from the result's column name. The typed SDK
// exposes column values by index but no column names at all, so the mapping is
// positional and must match the server's field list exactly. This list is
// transcribed from sql/rpl_replica.cc in the VillageSQL 8.4 tree that this
// extension is built against.
constexpr const char *const kColumns[] = {
    "Replica_IO_State", "Source_Host", "Source_User", "Source_Port",
    "Connect_Retry", "Source_Log_File", "Read_Source_Log_Pos", "Relay_Log_File",
    "Relay_Log_Pos", "Relay_Source_Log_File", "Replica_IO_Running",
    "Replica_SQL_Running", "Replicate_Do_DB", "Replicate_Ignore_DB",
    "Replicate_Do_Table", "Replicate_Ignore_Table", "Replicate_Wild_Do_Table",
    "Replicate_Wild_Ignore_Table", "Last_Errno", "Last_Error", "Skip_Counter",
    "Exec_Source_Log_Pos", "Relay_Log_Space", "Until_Condition", "Until_Log_File",
    "Until_Log_Pos", "Source_SSL_Allowed", "Source_SSL_CA_File",
    "Source_SSL_CA_Path", "Source_SSL_Cert", "Source_SSL_Cipher", "Source_SSL_Key",
    "Seconds_Behind_Source", "Source_SSL_Verify_Server_Cert", "Last_IO_Errno",
    "Last_IO_Error", "Last_SQL_Errno", "Last_SQL_Error",
    "Replicate_Ignore_Server_Ids", "Source_Server_Id", "Source_UUID",
    "Source_Info_File", "SQL_Delay", "SQL_Remaining_Delay",
    "Replica_SQL_Running_State", "Source_Retry_Count", "Source_Bind",
    "Last_IO_Error_Timestamp", "Last_SQL_Error_Timestamp", "Source_SSL_Crl",
    "Source_SSL_Crlpath", "Retrieved_Gtid_Set", "Executed_Gtid_Set",
    "Auto_Position", "Replicate_Rewrite_DB", "Channel_Name", "Source_TLS_Version",
    "Source_public_key_path", "Get_Source_public_key", "Network_Namespace"};

constexpr unsigned kColumnCount = static_cast<unsigned>(std::size(kColumns));
static_assert(kColumnCount == 60,
              "the transcribed SHOW REPLICA STATUS field list changed length");

// Looked up by name rather than written as literal offsets: a hand-maintained
// index would silently point at the wrong column if a name were ever inserted
// or removed, and the row-width guard below cannot catch a same-length edit.
constexpr unsigned index_of(std::string_view wanted) {
  for (unsigned i = 0; i < kColumnCount; ++i) {
    if (std::string_view(kColumns[i]) == wanted) return i;
  }
  return kColumnCount;  // triggers the static_asserts below
}

constexpr unsigned kSourceHostIdx = index_of("Source_Host");
constexpr unsigned kSourceUuidIdx = index_of("Source_UUID");
constexpr unsigned kChannelNameIdx = index_of("Channel_Name");
static_assert(kSourceHostIdx < kColumnCount, "Source_Host missing from kColumns");
static_assert(kSourceUuidIdx < kColumnCount, "Source_UUID missing from kColumns");
static_assert(kChannelNameIdx < kColumnCount, "Channel_Name missing from kColumns");

// The 60 metric names are fixed at build time; computing them per row would
// lowercase and reallocate the same 60 literals on every scrape.
const std::vector<std::string> &metric_names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    out.reserve(kColumnCount);
    for (const char *column : kColumns) {
      out.push_back("mysql_slave_status_" + valid_prometheus_name(column));
    }
    return out;
  }();
  return names;
}

}  // namespace

bool collect_slave_status(CollectorCtx &ctx) {
  bool layout_mismatch = false;
  unsigned observed_columns = 0;

  // Zero rows on a server with no replication configured is a successful scrape
  // that simply produces no metrics.
  const bool ok = query_rows(ctx, kQuery, [&](const sq::SqlQuery::Row &row) {
    // Positional mapping is only safe while the field list is the one above.
    // Refusing to emit on a mismatch turns a future column change into a
    // visible collector failure instead of silently mislabelled metrics.
    if (row.num_columns() != kColumnCount) {
      layout_mismatch = true;
      observed_columns = row.num_columns();
      return;
    }

    const std::vector<Label> labels = {
        {"master_host", std::string(col_str(row, kSourceHostIdx))},
        {"master_uuid", std::string(col_str(row, kSourceUuidIdx))},
        {"channel_name", std::string(col_str(row, kChannelNameIdx))},
        // Upstream's fourth label exists for MariaDB's named connections, which
        // MySQL 8.4 has no equivalent of; it is always empty here.
        {"connection_name", std::string()}};

    const std::vector<std::string> &names = metric_names();
    for (unsigned i = 0; i < kColumnCount; ++i) {
      double value = 0.0;
      if (!parse_status_value(col_str(row, i), &value)) continue;
      ctx.out->gauge_d(names[i], value, labels);
    }
  });
  if (!ok) return false;

  if (layout_mismatch) {
    *ctx.err = "SHOW REPLICA STATUS returned " + std::to_string(observed_columns) +
               " columns, expected " + std::to_string(kColumnCount) +
               "; positional column mapping is no longer valid";
    return false;
  }
  return true;
}

}  // namespace vsql_metrics
