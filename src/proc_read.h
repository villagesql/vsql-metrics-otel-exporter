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

#ifndef VSQL_METRICS_PROC_READ_H
#define VSQL_METRICS_PROC_READ_H

#include <string>

namespace vsql_metrics {

struct ProcSelf {
  long long rss_bytes = 0;
  long long open_fds = 0;
  long long max_fds = 0;
};

// Reads the calling process's resident set size and file-descriptor usage.
// Linux only: /proc/self/status for VmRSS, /proc/self/fd for the open count,
// getrlimit for the limit. Returns false with *err set on any read failure or
// on a platform without /proc; the caller decides whether that is an error
// (Linux) or a silent skip (everywhere else).
bool read_proc_self(ProcSelf &out, std::string *err);

}  // namespace vsql_metrics

#endif  // VSQL_METRICS_PROC_READ_H
