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

#include "scrape.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "http_endpoint.h"
#include "otlp_http.h"
#include "otlp_json.h"
#include "prom_text.h"

namespace vsql_metrics {
namespace scrape {

bool g_collect[kCollectorCount] = {};

// ids match mysqld_exporter's --collect.<id> flags; default_on mirrors its
// defaults, so a default install scrapes exactly what mysqld_exporter would.
// Sysvar names cannot contain '.', hence the underscored spelling.
// constexpr so the static_asserts below can check it; the extern declaration in
// scrape.h keeps it externally linked.
constexpr Collector kCollectors[kCollectorCount] = {
    {"global_status", "collect_global_status", true, &collect_global_status},
    {"global_variables", "collect_global_variables", true, &collect_global_variables},
    {"slave_status", "collect_slave_status", true, &collect_slave_status},
    {"info_schema.innodb_cmp", "collect_info_schema_innodb_cmp", true, &collect_innodb_cmp},
    {"info_schema.innodb_cmpmem", "collect_info_schema_innodb_cmpmem", true, &collect_innodb_cmpmem},
    {"info_schema.innodb_metrics", "collect_info_schema_innodb_metrics", false, &collect_innodb_metrics},
    {"info_schema.processlist", "collect_info_schema_processlist", false, &collect_processlist},
    {"info_schema.tables", "collect_info_schema_tables", false, &collect_info_schema_tables},
    {"perf_schema.eventsstatements", "collect_perf_schema_eventsstatements", false, &collect_events_statements},
    {"perf_schema.eventsstatementssum", "collect_perf_schema_stmt_sum", false, &collect_events_statements_sum},
    {"perf_schema.eventswaits", "collect_perf_schema_eventswaits", false, &collect_events_waits},
    {"perf_schema.tableiowaits", "collect_perf_schema_tableiowaits", false, &collect_table_io_waits},
    {"perf_schema.indexiowaits", "collect_perf_schema_indexiowaits", false, &collect_index_io_waits},
    {"perf_schema.file_events", "collect_perf_schema_file_events", false, &collect_file_events},
    {"perf_schema.tablelocks", "collect_perf_schema_tablelocks", false, &collect_table_locks},
    {"engine_innodb_status", "collect_engine_innodb_status", false, &collect_engine_innodb_status},
    {"binlog_size", "collect_binlog_size", false, &collect_binlog_size},
    // process and villagesql have no mysqld_exporter counterpart. On by
    // default: both are a handful of gauges per scrape.
    {"process", "collect_process", true, &collect_process},
    {"villagesql", "collect_villagesql", true, &collect_villagesql},
    {"host", "collect_host", true, &collect_host},
};

// Nothing but position ties a registry entry to its CollectorIdx, and getting
// that wrong would bind a collect_* variable to the wrong collector and
// mislabel last_scrape_error_collector -- both silently. Checked here, where
// the array definition is visible; spot-checking the ends and the middle is
// enough to catch an inserted or reordered row.
static_assert(std::string_view(kCollectors[kGlobalStatus].id) == "global_status",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kSlaveStatus].id) == "slave_status",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kInnodbCmpmem].id) ==
                  "info_schema.innodb_cmpmem",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kEventsStatementsSum].id) ==
                  "perf_schema.eventsstatementssum",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kBinlogSize].id) == "binlog_size",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kVillageSQL].id) == "villagesql",
              "kCollectors is out of step with the CollectorIdx enum");
static_assert(std::string_view(kCollectors[kHost].id) == "host",
              "kCollectors is out of step with the CollectorIdx enum");

// The server truncates a qualified system variable name at 64 characters, and
// does so silently -- the variable registers, but under a name nobody can guess
// and nobody can SET. That is how collect_perf_schema_eventsstatementssum, the
// direct transliteration of the upstream flag, ended up unreachable at 66
// characters and had to be shortened. Checked here so it cannot happen again.
constexpr std::size_t kQualifiedNameLimit = 64;
constexpr std::size_t kExtensionNameLength =
    std::string_view("vsql_metrics_otel_exporter").size();

constexpr bool sysvar_names_fit() {
  for (const Collector &collector : kCollectors) {
    const std::size_t qualified =
        kExtensionNameLength + 1 + std::string_view(collector.sysvar).size();
    if (qualified > kQualifiedNameLimit) return false;
  }
  return true;
}
static_assert(sysvar_names_fit(),
              "a collect_* variable name is too long: the server would silently "
              "truncate the qualified name and the variable could not be SET");

namespace {

const sq::SqlQueryCapability *g_cap = nullptr;
StringReader g_read_string = nullptr;
Config g_cfg{};
Status g_st{};

// startTimeUnixNano for every cumulative sum. Stamped at the first enable and
// never reset: the MySQL counters behind these metrics are process-lifetime, so
// moving it on a re-enable would claim a reset that did not happen.
std::uint64_t g_process_start_ns = 0;

// Start of the window every cumulative counter in this exporter accumulates
// over. Read from the server rather than assumed, because it is the server that
// owns the counters -- see resolve_server_start below.
std::uint64_t g_server_start_ns = 0;

// Wall clock of the last completed OTLP export. The push cadence is driven off
// this rather than off VEF_WAKEUP_PERIODIC, because a poll-fd wakeup restarts
// the worker's sleep: with a scraper connecting more often than the export
// interval, the periodic wakeup never arrives and a push keyed to it would
// never happen. Measured against a running server, a request every second
// against a two-second timer produced zero periodic wakeups in ten seconds.
std::uint64_t g_last_push_ns = 0;

// The /metrics listener, or -1. Owned by the worker thread.
int g_listen_fd = -1;

// Identifies this exporter as the OTLP instrumentation scope. The version is
// kept in step with manifest.json by hand; it names the producer of the
// metrics, not the server.
constexpr const char kScopeName[] = "vsql_metrics_otel_exporter";
constexpr const char kScopeVersion[] = "0.0.2";

// Used only when a tick fails so badly that the configured interval could not
// be read. Matches export_interval_ms's own default.
constexpr unsigned int kFallbackIntervalMs = 10000;

std::mutex g_log_mutex;
std::string g_last_logged_message;
std::time_t g_last_logged_at = 0;

// The server reads this memory directly for SHOW STATUS while the worker writes
// it. The worker is the only writer, so relaxed is sufficient; the load on the
// server's side is a plain aligned 8-byte read that cannot tear.
void store(long long *p, long long v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }

void add(long long *p, long long v) { __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }

// Reads a string sysvar safely. See StringReader in scrape.h for why the
// char** the SDK binds must not be dereferenced from this thread.
std::string cfg_str(const char *var_name) {
  std::string value;
  if (g_read_string != nullptr) g_read_string(var_name, &value);
  return value;
}

// The server keeps every registered sysvar inside its declared [min, max], and
// each minimum here is at least 1, so there is no fallback to apply.
long long cfg_int(long long *p) { return *p; }

std::uint64_t wall_unix_nano() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Status vars carry numeric failure classification; the human-readable text has
// nowhere to go in SQL because the SDK has no string status var. It goes to the
// error log instead, suppressing an identical repeat within a minute so a
// permanently unreachable collector cannot flood it.
void log_rate_limited(const std::string &message) {
  const std::time_t now = std::time(nullptr);
  {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (message == g_last_logged_message && now - g_last_logged_at < 60) return;
    g_last_logged_message = message;
    g_last_logged_at = now;
  }
  std::fprintf(stderr, "[vsql_metrics_otel_exporter] %s\n", message.c_str());
}

// Resolves when the server started, which is when its counters began
// accumulating. Read once: the extension is loaded into the server, so the
// server cannot restart without unloading this code with it -- unlike the
// instrument list, this value provably cannot change while we run.
void resolve_server_start(const sq::Session &session, std::uint64_t now_ns) {
  if (g_server_start_ns != 0) return;

  long long uptime_secs = 0;
  auto result =
      session
          .sql("SELECT VARIABLE_VALUE FROM performance_schema.global_status "
               "WHERE VARIABLE_NAME = 'Uptime'")
          .for_each([&uptime_secs](const sq::SqlQuery::Row &row) {
            uptime_secs = row.column_int(0);
          });
  if (result.has_error() || uptime_secs <= 0) return;

  const std::uint64_t uptime_ns =
      static_cast<std::uint64_t>(uptime_secs) * 1000000000ull;
  g_server_start_ns = (now_ns > uptime_ns) ? now_ns - uptime_ns : 0;
}

void export_batch(const MetricBatch &batch, std::uint64_t now_ns) {
  const std::string endpoint = cfg_str("otlp_endpoint");
  if (endpoint.empty()) {
    // Documented as the way to hold the worker running with sending switched
    // off, so it is not counted as an export error.
    log_rate_limited("otlp_endpoint is empty; not exporting");
    return;
  }

  std::vector<std::string> headers;
  std::string header_error;
  const std::string header_spec = cfg_str("otlp_headers");
  if (!otlp_http::parse_headers(header_spec, &headers, &header_error)) {
    add(g_st.export_errors_total, 1);
    log_rate_limited(header_error);
    return;
  }

  otlp_json::Options opt;
  opt.service_name = cfg_str("service_name");
  if (opt.service_name.empty()) opt.service_name = "villagesql";
  opt.service_instance_id = cfg_str("service_instance_id");
  opt.scope_name = kScopeName;
  opt.scope_version = kScopeVersion;
  opt.max_datapoints =
      static_cast<std::size_t>(cfg_int(g_cfg.max_datapoints_per_request));
  opt.max_body_bytes = static_cast<std::size_t>(cfg_int(g_cfg.max_body_bytes));

  // Falls back to process start only if the server never answered, which would
  // mean the scrape failed anyway.
  const std::uint64_t start_ns =
      (g_server_start_ns != 0) ? g_server_start_ns : g_process_start_ns;
  const std::vector<std::string> bodies =
      otlp_json::serialize(batch, now_ns, start_ns, opt);
  store(g_st.chunks_last_export, static_cast<long long>(bodies.size()));
  if (bodies.empty()) return;

  const std::string url = otlp_http::join_url(endpoint, "/v1/metrics");
  const long timeout = static_cast<long>(cfg_int(g_cfg.http_timeout_secs));

  long long bytes_sent = 0;
  bool all_ok = true;
  for (const std::string &body : bodies) {
    const otlp_http::Response response =
        otlp_http::post_json(url, body, headers, timeout);
    store(g_st.last_export_http_status, response.http_status);
    store(g_st.last_export_curl_errno, response.curl_error);
    if (response.ok) {
      bytes_sent += static_cast<long long>(body.size());
      continue;
    }
    // A failed chunk does not abandon the rest: a partial export beats none.
    all_ok = false;
    add(g_st.export_errors_total, 1);
    log_rate_limited(response.message);
  }

  add(g_st.bytes_exported_total, bytes_sent);
  if (all_ok) {
    add(g_st.exports_total, 1);
    add(g_st.datapoints_exported_total,
        static_cast<long long>(batch.point_count()));
  }
}

// Reads the server into `batch`. Shared by both endpoints, so the push and the
// pull path can never disagree about names, labels or values -- they differ
// only in how the same batch is serialized.
void collect(struct vef_thread_handle_t *handle, MetricBatch &batch,
             std::uint64_t now_ns, long long *failed_collector_out) {
  ScrapeConfig sc;
  // processlist_min_time is the one tunable whose range starts at 0, and 0 is
  // meaningful (count every thread), so it is read directly.
  sc.processlist_min_time = *g_cfg.processlist_min_time;
  sc.stmt_limit = cfg_int(g_cfg.eventsstatements_limit);
  sc.stmt_timelimit = cfg_int(g_cfg.eventsstatements_timelimit);
  sc.stmt_digest_text_limit = cfg_int(g_cfg.eventsstatements_digest_text_limit);
  sc.cores_dir = cfg_str("cores_dir");

  long long failed_collector = 0;

  // One session for the whole tick. A collector whose statement errors does not
  // poison it: the capability creates a fresh Srv_session per execute, so the
  // failure is scoped to that statement.
  auto session = g_cap->open(handle);
  if (!session) {
    batch.gauge_i("mysql_up", 0);
    add(g_st.scrape_errors_total, 1);
    log_rate_limited("could not open a SQL session for the scrape");
  } else {
    batch.gauge_i("mysql_up", 1);
    resolve_server_start(session, now_ns);

    for (int i = 0; i < kCollectorCount; ++i) {
      if (!g_collect[i]) continue;
      const Collector &collector = kCollectors[i];

      std::string err;
      CollectorCtx ctx{&session, &batch, &sc, &err};

      const auto collector_started = std::chrono::steady_clock::now();
      // One collector must never take the scrape down with it. query_rows
      // already contains anything thrown from a row handler; this covers the
      // collector body outside that, such as the schema-list buffering in
      // info_schema.tables.
      bool ok = false;
      try {
        ok = collector.scrape(ctx);
      } catch (const std::exception &e) {
        err = e.what();
      } catch (...) {
        err = "collector threw a non-standard exception";
      }
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                        collector_started)
              .count();

      batch.gauge_i("mysql_exporter_collector_success", ok ? 1 : 0,
                    {{"collector", collector.id}});
      batch.gauge_d("mysql_exporter_collector_duration_seconds", seconds,
                    {{"collector", collector.id}});

      if (!ok) {
        add(g_st.scrape_errors_total, 1);
        failed_collector = i + 1;  // 1-based; 0 means "none failed"
        log_rate_limited(std::string("collector ") + collector.id + " failed: " + err);
      }
    }
  }

  if (batch.kind_conflicts() > 0) {
    log_rate_limited("two collectors emitted the same metric name with "
                     "different types; some data points were serialized using "
                     "the first collector's type");
  }
  *failed_collector_out = failed_collector;
}

// The push path: collect, then export over OTLP.
void run_tick(struct vef_thread_handle_t *handle) {
  const std::uint64_t now_ns = wall_unix_nano();
  const auto started = std::chrono::steady_clock::now();

  MetricBatch batch;
  long long failed_collector = 0;
  collect(handle, batch, now_ns, &failed_collector);

  // Measured before the export: this is how long reading the server took, not
  // how long the collector took to answer. Otherwise an unreachable endpoint
  // would report a multi-second "scrape" that in fact took milliseconds.
  const long long elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count();

  store(g_st.last_scrape_error_collector, failed_collector);
  store(g_st.last_datapoint_count, static_cast<long long>(batch.point_count()));
  store(g_st.last_scrape_duration_us, elapsed_us);
  store(g_st.last_scrape_utime, static_cast<long long>(now_ns / 1000));
  add(g_st.scrapes_total, 1);

  export_batch(batch, now_ns);
  g_last_push_ns = now_ns;
}

// The pull path: collect and answer one scraper, in Prometheus text.
//
// Runs on the worker thread, so it is bounded at every step -- the export
// cannot proceed while this is in progress.
void serve_scrape(struct vef_thread_handle_t *handle, int client_fd) {
  const auto started = std::chrono::steady_clock::now();

  const http_endpoint::Route route =
      http_endpoint::read_request(client_fd, cfg_str("prometheus_auth_token"));

  if (route == http_endpoint::Route::kUnauthorized) {
    add(g_st.prometheus_denied_total, 1);
    http_endpoint::respond_status(client_fd, 401, "Unauthorized");
    return;
  }
  if (route == http_endpoint::Route::kNotFound) {
    http_endpoint::respond_status(client_fd, 404, "Not Found");
    return;
  }

  MetricBatch batch;
  long long failed_collector = 0;
  collect(handle, batch, wall_unix_nano(), &failed_collector);

  const std::string body = prom_text::serialize(batch);
  http_endpoint::respond_metrics(client_fd, body);

  add(g_st.prometheus_requests_total, 1);
  add(g_st.scrapes_total, 1);
  store(g_st.last_scrape_error_collector, failed_collector);
  store(g_st.last_datapoint_count, static_cast<long long>(batch.point_count()));
  store(g_st.prometheus_last_serve_us,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

// Opens or closes the listener to match the current configuration. Returns the
// value to report as poll_fd: a new descriptor when one was just opened, -1 to
// clear a closed one, and 0 to leave the current registration alone.
int reconcile_listener() {
  const bool wanted = (g_cfg.prometheus_enabled != nullptr) &&
                      *g_cfg.prometheus_enabled;

  if (wanted && g_listen_fd < 0) {
    std::string bind_address = cfg_str("prometheus_bind_address");
    if (bind_address.empty()) bind_address = "127.0.0.1";
    std::string error;
    const int fd = http_endpoint::open_listener(
        bind_address, static_cast<int>(cfg_int(g_cfg.prometheus_port)), &error);
    if (fd < 0) {
      add(g_st.prometheus_errors_total, 1);
      log_rate_limited("prometheus endpoint: " + error);
      return 0;
    }
    g_listen_fd = fd;
    store(g_st.prometheus_listening, 1);
    return fd;
  }

  if (!wanted && g_listen_fd >= 0) {
    http_endpoint::close_listener(g_listen_fd);
    g_listen_fd = -1;
    store(g_st.prometheus_listening, 0);
    return -1;  // -1 clears the registration; 0 would leave a dead fd polled
  }
  return 0;
}

// Whether an export is due. Kept separate from the sleep calculation below:
// folding the two together made "never pushed yet" indistinguishable from "a
// full interval remains", so the very first push never happened and the push
// path stayed dead for the life of the process.
bool push_due(unsigned int interval_ms) {
  if (g_last_push_ns == 0) return true;  // nothing exported yet -- due now
  const std::uint64_t now_ns = wall_unix_nano();
  if (now_ns <= g_last_push_ns) return false;
  return (now_ns - g_last_push_ns) / 1000000ull >= interval_ms;
}

// Milliseconds until the next export is due. Returning the whole interval on
// every wakeup would let frequent scrapes push the export indefinitely into the
// future, so what is returned is the time remaining, never zero (which the
// server reads as "keep the current interval").
unsigned int sleep_until_next_push(unsigned int interval_ms) {
  if (g_last_push_ns == 0) return 1;
  const std::uint64_t now_ns = wall_unix_nano();
  if (now_ns <= g_last_push_ns) return interval_ms;
  const std::uint64_t elapsed_ms = (now_ns - g_last_push_ns) / 1000000ull;
  if (elapsed_ms >= interval_ms) return 1;
  return static_cast<unsigned int>(interval_ms - elapsed_ms);
}

// The scrape itself. Kept separate from on_wakeup so the exception guard there
// covers every part of it, not just the per-collector call.
vef_next_wakeup_t run_wakeup(vef_wakeup_reason_t reason,
                             struct vef_thread_handle_t *handle) {
  // Read fresh every tick. This is the whole mechanism by which an interval
  // change takes effect: the server only re-reads sleep_ms from this return
  // value, so there is nothing to signal and nothing to race.
  const unsigned int interval =
      static_cast<unsigned int>(cfg_int(g_cfg.export_interval_ms));

  if (reason == VEF_WAKEUP_ENABLE) {
    // The handle is null here and this runs on the SET GLOBAL caller's thread,
    // so no SQL is possible. Setup only -- but the socket does belong here: the
    // server calls ENABLE synchronously so that a bind failure can be reported
    // before SET GLOBAL returns instead of being discovered later.
    otlp_http::global_init();
    if (g_process_start_ns == 0) g_process_start_ns = wall_unix_nano();
    // The listener is NOT opened here, even though the SDK invites it: ENABLE
    // runs inside the SET GLOBAL that switched the worker on, and that
    // statement already holds LOCK_global_system_variables. Opening the socket
    // needs prometheus_bind_address, reading a string system variable goes
    // through the server's component reader, and that reader takes the same
    // lock -- which self-deadlocks the connection and, because every new
    // session reads system variables, takes the whole server with it. Observed
    // exactly that way: SET GLOBAL never returned and the server had to be
    // killed. The socket is opened on the first worker wakeup instead, 250ms
    // later, off that thread and outside the lock.
    //
    // The cost is that a bind failure is no longer reported by SET GLOBAL. It
    // surfaces in prometheus_errors_total, prometheus_listening and the error
    // log.
    return {std::min<unsigned int>(interval, 250u), 0};
  }

  if (reason == VEF_WAKEUP_DISABLE) {
    // No final export: DISABLE also fires on server shutdown, where a blocking
    // HTTP request would stall it for up to http_timeout_secs.
    if (g_listen_fd >= 0) {
      http_endpoint::close_listener(g_listen_fd);
      g_listen_fd = -1;
      store(g_st.prometheus_listening, 0);
    }
    return {};
  }

  if (g_cap == nullptr || handle == nullptr) return {interval, 0};

  if (reason == VEF_WAKEUP_POLL_FD) {
    const int client = http_endpoint::accept_client(g_listen_fd);
    if (client >= 0) {
      serve_scrape(handle, client);
      http_endpoint::close_listener(client);
    }
  }

  // Both wakeup kinds fall through to here, so the export keeps its cadence
  // however often scrapers connect.
  if (push_due(interval)) run_tick(handle);

  // Picks up a prometheus_enabled or port change without needing the worker
  // restarted; on a poll-fd wakeup this is normally a no-op returning 0.
  const int poll_fd = reconcile_listener();
  return {sleep_until_next_push(interval), poll_fd};
}

}  // namespace

void init(const sq::SqlQueryCapability *cap, StringReader read_string,
          const Config &cfg, const Status &st) {
  g_cap = cap;
  g_read_string = read_string;
  g_cfg = cfg;
  g_st = st;
  // g_collect is not seeded here: sysvar registration writes each collector's
  // default into it, and doing it twice would risk clobbering a value the
  // server had already applied.
}

// The server invokes the worker through a function pointer declared inside
// extern "C", so nothing may unwind out of it. Most of a tick allocates --
// building the batch, serializing bodies that can reach megabytes, copying
// sysvar strings -- so guarding only the per-collector call left most of the
// path uncovered. Returning a usable interval on failure keeps the worker
// ticking instead of parking it on the server's 24-hour default.
vef_next_wakeup_t on_wakeup(vef_wakeup_reason_t reason,
                            struct vef_thread_handle_t *handle) noexcept {
  try {
    return run_wakeup(reason, handle);
  } catch (...) {
    // Deliberately not log_rate_limited(): that allocates and takes a mutex,
    // and the failure this guard exists for is an allocation failure. A throw
    // from the handler of a noexcept function is std::terminate, so the
    // recovery path must not be able to throw. fputs is enough.
    std::fputs("[vsql_metrics_otel_exporter] scrape worker failed; skipping "
               "this interval\n",
               stderr);
  }
  return {kFallbackIntervalMs, 0};
}

}  // namespace scrape
}  // namespace vsql_metrics
