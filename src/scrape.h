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

#ifndef VSQL_METRICS_SCRAPE_H
#define VSQL_METRICS_SCRAPE_H

#include <villagesql/preview/sql_query.h>
#include <villagesql/preview/thread_worker.h>

#include <string>

#include "collector.h"

namespace vsql_metrics {
namespace scrape {

// The collector registry, plus the storage the sysvar and status-var tables in
// extension.cc point at. Defined here so the registry and its per-collector
// state stay in one translation unit; extension.cc supplies only the visible
// SQL names.
extern bool g_collect[kCollectorCount];
extern const Collector kCollectors[kCollectorCount];

// Reads the current value of one of this extension's string system variables.
//
// String sysvars must NOT be read by dereferencing the char** the SDK binds
// them to. The server registers them PLUGIN_VAR_MEMALLOC and, on SET GLOBAL,
// stores the new pointer and then frees the old one -- so a worker holding the
// old pointer reads freed memory. The server's own source says as much:
// "a concurrent SET may free the old string value". Going through the sysvar
// capability's reader instead takes the value under the server's lock and hands
// back a private copy.
//
// Returns false if the variable could not be read, leaving *out untouched.
using StringReader = bool (*)(const char *var_name, std::string *out);

// Pointers to the numeric sysvar-backed globals in extension.cc. Held as
// pointers, not values, so a SET GLOBAL is picked up on the next tick with no
// callback and no cross-thread signalling. Integers are safe to read this way;
// only strings are reallocated behind the extension's back.
struct Config {
  long long *export_interval_ms;
  long long *http_timeout_secs;
  long long *max_datapoints_per_request;
  long long *max_body_bytes;
  long long *processlist_min_time;
  long long *eventsstatements_limit;
  long long *eventsstatements_timelimit;
  long long *eventsstatements_digest_text_limit;
};

// Pointers to the status-var-backed counters in extension.cc. The server reads
// this memory directly on SHOW STATUS, so the worker updates it with relaxed
// atomics rather than through any accessor.
struct Status {
  long long *scrapes_total;
  long long *scrape_errors_total;
  long long *exports_total;
  long long *export_errors_total;
  long long *datapoints_exported_total;
  long long *bytes_exported_total;
  long long *last_scrape_utime;
  long long *last_scrape_duration_us;
  long long *last_export_http_status;
  long long *last_export_curl_errno;
  long long *last_scrape_error_collector;
  long long *last_datapoint_count;
  long long *chunks_last_export;
};

void init(const sq::SqlQueryCapability *cap, StringReader read_string,
          const Config &cfg, const Status &st);

// noexcept because the server calls the worker through a function pointer
// declared inside extern "C"; nothing may unwind out of it.
vef_next_wakeup_t on_wakeup(vef_wakeup_reason_t reason,
                            struct vef_thread_handle_t *handle) noexcept;

}  // namespace scrape
}  // namespace vsql_metrics

#endif  // VSQL_METRICS_SCRAPE_H
