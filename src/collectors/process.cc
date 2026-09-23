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

#include "collector.h"
#include "proc_read.h"

namespace vsql_metrics {

// The server process's own memory and file-descriptor usage, under the
// conventional Prometheus process_* names.
bool collect_process(CollectorCtx &ctx) {
#ifdef __linux__
  ProcSelf p;
  if (!read_proc_self(p, ctx.err)) return false;
  ctx.out->gauge_i("process_resident_memory_bytes", p.rss_bytes);
  ctx.out->gauge_i("process_open_fds", p.open_fds);
  ctx.out->gauge_i("process_max_fds", p.max_fds);
  return true;
#else
  // No /proc: emit nothing rather than fail every scrape on macOS dev builds.
  (void)ctx;
  return true;
#endif
}

}  // namespace vsql_metrics
