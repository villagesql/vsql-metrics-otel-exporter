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

// vsql_metrics_otel_exporter: VillageSQL server metrics, named exactly as
// mysqld_exporter names them, pushed as OpenTelemetry metrics over OTLP/HTTP to
// a collector's /v1/metrics on a timer.
//
// This file owns only the SDK capability objects, the sysvar and status-var
// tables, and the entry-point composition. The scrape loop lives in scrape.cc
// and the per-collector mappings in collectors/.

#include <villagesql/preview/sql_query.h>
#include <villagesql/preview/status_var.h>
#include <villagesql/preview/sys_var.h>
#include <villagesql/preview/thread_worker.h>
#include <villagesql/vsql.h>

#include <string>
#include <string_view>

#include "scrape.h"

namespace sq = vsql::preview_sql_query;
namespace stv = vsql::preview_status_var;
namespace sv = vsql::preview_sys_var;
namespace tw = vsql::preview_thread_worker;

namespace {

using vsql_metrics::kCollectorCount;
namespace scrape = vsql_metrics::scrape;

// Integer sys/status vars are declared as `long long` to match the SDK contract
// (make_int takes `long long *`). int64_t is `long` on LP64 Linux and
// `long long` on macOS, so int64_t compiles on macOS but fails to bind here.
//
// There is deliberately no double-typed sysvar anywhere: the server's component
// variable registration has no PLUGIN_VAR_DOUBLE, and a failed registration
// rolls back every variable registered before it.

char *g_otlp_endpoint = nullptr;
char *g_otlp_headers = nullptr;
char *g_service_name = nullptr;
char *g_service_instance_id = nullptr;
long long g_export_interval_ms = 10000;
long long g_http_timeout_secs = 10;
long long g_max_datapoints_per_request = 2000;
long long g_max_body_bytes = 4 * 1024 * 1024;

bool g_prometheus_enabled = false;
long long g_prometheus_port = 9104;
char *g_prometheus_bind_address = nullptr;
char *g_prometheus_auth_token = nullptr;

long long g_processlist_min_time = 0;
long long g_eventsstatements_limit = 250;
long long g_eventsstatements_timelimit = 86400;
long long g_eventsstatements_digest_text_limit = 120;

// The collector toggles, their SQL names and their defaults all come from the
// registry in scrape.cc, so a collector is described in exactly one place.
const char *collect_name(int idx) { return scrape::kCollectors[idx].sysvar; }
bool *collect_flag(int idx) { return &scrape::g_collect[idx]; }
bool collect_default(int idx) { return scrape::kCollectors[idx].default_on; }

auto SYS_VARS = sv::make_capability({
    sv::make_str("otlp_endpoint",
                 "OTLP/HTTP collector base URL, e.g. http://host:4318 "
                 "(/v1/metrics is appended; empty disables sending)",
                 &g_otlp_endpoint, "http://localhost:4318"),
    sv::make_str("otlp_headers",
                 "Extra HTTP headers as comma-separated Key=Value pairs, for "
                 "collectors requiring authentication",
                 &g_otlp_headers, ""),
    sv::make_str("service_name", "OTLP resource attribute service.name",
                 &g_service_name, "villagesql"),
    sv::make_str("service_instance_id",
                 "OTLP resource attribute service.instance.id (empty omits it)",
                 &g_service_instance_id, ""),
    sv::make_int("export_interval_ms", "Scrape and export this often (ms)",
                 &g_export_interval_ms, 10000, 1000, 3600000),
    sv::make_int("http_timeout_secs", "Timeout for one OTLP export (seconds)",
                 &g_http_timeout_secs, 10, 1, 300),
    sv::make_int("max_datapoints_per_request",
                 "Split the export into several requests above this many data "
                 "points",
                 &g_max_datapoints_per_request, 2000, 100, 1000000),
    sv::make_int("max_body_bytes",
                 "Size backstop for one export request body (bytes)",
                 &g_max_body_bytes, 4 * 1024 * 1024, 65536, 134217728),

    // The pull endpoint. Off by default: it opens a listening socket in the
    // server process, which is a change in exposure that should be opted into
    // rather than inherited.
    sv::make_bool("prometheus_enabled",
                  "Serve Prometheus text metrics on GET /metrics",
                  &g_prometheus_enabled, false),
    sv::make_int("prometheus_port", "TCP port for the /metrics endpoint",
                 &g_prometheus_port, 9104, 1024, 65535),
    sv::make_str("prometheus_bind_address",
                 "IPv4 address to bind the /metrics endpoint to",
                 &g_prometheus_bind_address, "127.0.0.1"),
    sv::make_str("prometheus_auth_token",
                 "If set, /metrics requires Authorization: Bearer <token>",
                 &g_prometheus_auth_token, ""),

    // One toggle per collector, mirroring mysqld_exporter's --collect.* flags.
    // Its flag names contain dots, which a sysvar name cannot, so
    // --collect.info_schema.innodb_cmp becomes collect_info_schema_innodb_cmp.
    sv::make_bool(collect_name(vsql_metrics::kGlobalStatus),
                  "Collect SHOW GLOBAL STATUS",
                  collect_flag(vsql_metrics::kGlobalStatus),
                  collect_default(vsql_metrics::kGlobalStatus)),
    sv::make_bool(collect_name(vsql_metrics::kGlobalVariables),
                  "Collect SHOW GLOBAL VARIABLES",
                  collect_flag(vsql_metrics::kGlobalVariables),
                  collect_default(vsql_metrics::kGlobalVariables)),
    sv::make_bool(collect_name(vsql_metrics::kSlaveStatus),
                  "Collect SHOW REPLICA STATUS",
                  collect_flag(vsql_metrics::kSlaveStatus),
                  collect_default(vsql_metrics::kSlaveStatus)),
    sv::make_bool(collect_name(vsql_metrics::kInnodbCmp),
                  "Collect information_schema.innodb_cmp",
                  collect_flag(vsql_metrics::kInnodbCmp),
                  collect_default(vsql_metrics::kInnodbCmp)),
    sv::make_bool(collect_name(vsql_metrics::kInnodbCmpmem),
                  "Collect information_schema.innodb_cmpmem",
                  collect_flag(vsql_metrics::kInnodbCmpmem),
                  collect_default(vsql_metrics::kInnodbCmpmem)),
    sv::make_bool(collect_name(vsql_metrics::kInnodbMetrics),
                  "Collect information_schema.innodb_metrics",
                  collect_flag(vsql_metrics::kInnodbMetrics),
                  collect_default(vsql_metrics::kInnodbMetrics)),
    sv::make_bool(collect_name(vsql_metrics::kProcesslist),
                  "Collect information_schema.processlist",
                  collect_flag(vsql_metrics::kProcesslist),
                  collect_default(vsql_metrics::kProcesslist)),
    sv::make_bool(collect_name(vsql_metrics::kInfoSchemaTables),
                  "Collect per-table size and row counts (high cardinality)",
                  collect_flag(vsql_metrics::kInfoSchemaTables),
                  collect_default(vsql_metrics::kInfoSchemaTables)),
    sv::make_bool(collect_name(vsql_metrics::kEventsStatements),
                  "Collect per-digest statement summaries",
                  collect_flag(vsql_metrics::kEventsStatements),
                  collect_default(vsql_metrics::kEventsStatements)),
    sv::make_bool(collect_name(vsql_metrics::kEventsStatementsSum),
                  "Collect server-wide statement summary totals",
                  collect_flag(vsql_metrics::kEventsStatementsSum),
                  collect_default(vsql_metrics::kEventsStatementsSum)),
    sv::make_bool(collect_name(vsql_metrics::kEventsWaits),
                  "Collect wait-event summaries",
                  collect_flag(vsql_metrics::kEventsWaits),
                  collect_default(vsql_metrics::kEventsWaits)),
    sv::make_bool(collect_name(vsql_metrics::kTableIoWaits),
                  "Collect per-table I/O wait summaries",
                  collect_flag(vsql_metrics::kTableIoWaits),
                  collect_default(vsql_metrics::kTableIoWaits)),
    sv::make_bool(collect_name(vsql_metrics::kIndexIoWaits),
                  "Collect per-index I/O wait summaries",
                  collect_flag(vsql_metrics::kIndexIoWaits),
                  collect_default(vsql_metrics::kIndexIoWaits)),
    sv::make_bool(collect_name(vsql_metrics::kFileEvents),
                  "Collect file I/O event summaries",
                  collect_flag(vsql_metrics::kFileEvents),
                  collect_default(vsql_metrics::kFileEvents)),
    sv::make_bool(collect_name(vsql_metrics::kTableLocks),
                  "Collect table lock wait summaries",
                  collect_flag(vsql_metrics::kTableLocks),
                  collect_default(vsql_metrics::kTableLocks)),
    sv::make_bool(collect_name(vsql_metrics::kEngineInnodbStatus),
                  "Collect counters parsed from SHOW ENGINE INNODB STATUS",
                  collect_flag(vsql_metrics::kEngineInnodbStatus),
                  collect_default(vsql_metrics::kEngineInnodbStatus)),
    sv::make_bool(collect_name(vsql_metrics::kBinlogSize),
                  "Collect binary log file sizes",
                  collect_flag(vsql_metrics::kBinlogSize),
                  collect_default(vsql_metrics::kBinlogSize)),
    sv::make_bool(collect_name(vsql_metrics::kProcess),
                  "Collect the server process's memory and fd gauges (Linux)",
                  collect_flag(vsql_metrics::kProcess),
                  collect_default(vsql_metrics::kProcess)),
    sv::make_bool(collect_name(vsql_metrics::kVillageSQL),
                  "Collect VillageSQL extension-framework state",
                  collect_flag(vsql_metrics::kVillageSQL),
                  collect_default(vsql_metrics::kVillageSQL)),

    sv::make_int("processlist_min_time",
                 "Only count processlist threads at least this old (seconds)",
                 &g_processlist_min_time, 0, 0, 86400),
    sv::make_int("eventsstatements_limit",
                 "Maximum statement digests to report per scrape",
                 &g_eventsstatements_limit, 250, 1, 10000),
    sv::make_int("eventsstatements_timelimit",
                 "Only report digests last seen within this many seconds",
                 &g_eventsstatements_timelimit, 86400, 1, 604800),
    sv::make_int("eventsstatements_digest_text_limit",
                 "Truncate digest_text label values to this many characters",
                 &g_eventsstatements_digest_text_limit, 120, 1, 4096),
});

// Failure classification is numeric because the SDK has no string status var;
// the corresponding message goes to the server error log. See README.
long long g_scrapes_total = 0;
long long g_scrape_errors_total = 0;
long long g_exports_total = 0;
long long g_export_errors_total = 0;
long long g_datapoints_exported_total = 0;
long long g_bytes_exported_total = 0;
long long g_last_scrape_utime = 0;
long long g_last_scrape_duration_us = 0;
long long g_last_export_http_status = 0;
long long g_last_export_curl_errno = 0;
long long g_last_scrape_error_collector = 0;
long long g_last_datapoint_count = 0;
long long g_chunks_last_export = 0;
long long g_prometheus_listening = 0;
long long g_prometheus_requests_total = 0;
long long g_prometheus_denied_total = 0;
long long g_prometheus_errors_total = 0;
long long g_prometheus_last_serve_us = 0;

auto STATUS_VARS = stv::make_capability({
    stv::make_int("scrapes_total", &g_scrapes_total),
    stv::make_int("scrape_errors_total", &g_scrape_errors_total),
    stv::make_int("exports_total", &g_exports_total),
    stv::make_int("export_errors_total", &g_export_errors_total),
    stv::make_int("datapoints_exported_total", &g_datapoints_exported_total),
    stv::make_int("bytes_exported_total", &g_bytes_exported_total),
    stv::make_int("last_scrape_utime", &g_last_scrape_utime),
    stv::make_int("last_scrape_duration_us", &g_last_scrape_duration_us),
    stv::make_int("last_export_http_status", &g_last_export_http_status),
    stv::make_int("last_export_curl_errno", &g_last_export_curl_errno),
    stv::make_int("last_scrape_error_collector", &g_last_scrape_error_collector),
    stv::make_int("last_datapoint_count", &g_last_datapoint_count),
    stv::make_int("chunks_last_export", &g_chunks_last_export),
    stv::make_int("prometheus_listening", &g_prometheus_listening),
    stv::make_int("prometheus_requests_total", &g_prometheus_requests_total),
    stv::make_int("prometheus_denied_total", &g_prometheus_denied_total),
    stv::make_int("prometheus_errors_total", &g_prometheus_errors_total),
    stv::make_int("prometheus_last_serve_us", &g_prometheus_last_serve_us),
});

sq::SqlQueryCapability SQL_QUERY;

// Reads a string system variable through the server rather than by
// dereferencing the char** above. See StringReader in scrape.h: the server
// frees the previous string on SET GLOBAL, so the raw pointer is not safe to
// read from the worker thread.
bool read_string_var(const char *var_name, std::string *out) {
  // The capability reports true on error.
  return !SYS_VARS.get("vsql_metrics_otel_exporter", var_name, *out);
}

// Binds the config and status pointers into the scrape driver once. Static
// initialization order does not matter: this captures the ADDRESSES of the
// globals above, which are fixed at link time, never their values.
const bool g_inited = [] {
  scrape::init(&SQL_QUERY, &read_string_var,
               scrape::Config{&g_export_interval_ms, &g_http_timeout_secs,
                              &g_max_datapoints_per_request, &g_max_body_bytes,
                              &g_prometheus_enabled, &g_prometheus_port,
                              &g_processlist_min_time,
                              &g_eventsstatements_limit,
                              &g_eventsstatements_timelimit,
                              &g_eventsstatements_digest_text_limit},
               scrape::Status{&g_scrapes_total, &g_scrape_errors_total,
                              &g_exports_total, &g_export_errors_total,
                              &g_datapoints_exported_total,
                              &g_bytes_exported_total, &g_last_scrape_utime,
                              &g_last_scrape_duration_us,
                              &g_last_export_http_status,
                              &g_last_export_curl_errno,
                              &g_last_scrape_error_collector,
                              &g_last_datapoint_count, &g_chunks_last_export,
                              &g_prometheus_listening,
                              &g_prometheus_requests_total,
                              &g_prometheus_denied_total,
                              &g_prometheus_errors_total,
                              &g_prometheus_last_serve_us});
  return true;
}();

vef_next_wakeup_t scrape_worker(vef_wakeup_reason_t reason,
                                struct vef_thread_handle_t *thread, void *) {
  return scrape::on_wakeup(reason, thread);
}

// The second argument names the boolean sysvar that switches the worker on. The
// capability registers it itself, so it must not appear in SYS_VARS.
//
// Deliberately not called "enabled". The server matches a worker to its switch
// by the UNQUALIFIED variable name (services/preview/thread_worker.cc:351-364),
// so two installed extensions that both call theirs "enabled" silently drive
// each other's worker -- observed directly against a running server. A
// distinctive name is the only defence available from this side.
tw::ThreadWorkerCapability<&scrape_worker> WORKER{"scrape", "scrape_enabled"};

}  // namespace

VEF_GENERATE_ENTRY_POINTS(vsql::make_extension()
                              .with(SYS_VARS)
                              .with(STATUS_VARS)
                              .with(SQL_QUERY)
                              .with(WORKER))
