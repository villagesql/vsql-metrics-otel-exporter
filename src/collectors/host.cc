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

#ifdef __linux__
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

namespace vsql_metrics {

#ifdef __linux__
namespace {

// The aggregate "cpu" line of /proc/stat: jiffies per mode since boot.
void emit_cpu(MetricBatch &out) {
  std::ifstream f("/proc/stat");
  std::string line;
  if (!std::getline(f, line) || line.rfind("cpu ", 0) != 0) return;
  std::istringstream in(line);
  std::string label;
  in >> label;
  static const char *const kModes[] = {"user", "nice",    "system", "idle",
                                       "iowait", "irq",   "softirq", "steal"};
  const double ticks_per_second = static_cast<double>(sysconf(_SC_CLK_TCK));
  for (const char *mode : kModes) {
    unsigned long long jiffies = 0;
    if (!(in >> jiffies)) break;
    out.counter_d("villagesql_host_cpu_seconds_total",
                  static_cast<double>(jiffies) / ticks_per_second,
                  {{"mode", mode}});
  }
}

// The four /proc/meminfo rows that say whether the machine is running out.
void emit_meminfo(MetricBatch &out) {
  std::ifstream f("/proc/meminfo");
  std::string key, rest;
  unsigned long long kb = 0;
  while (f >> key >> kb) {
    std::getline(f, rest);  // the unit
    const char *name = nullptr;
    if (key == "MemTotal:") name = "villagesql_host_memory_total_bytes";
    else if (key == "MemAvailable:") name = "villagesql_host_memory_available_bytes";
    else if (key == "SwapTotal:") name = "villagesql_host_swap_total_bytes";
    else if (key == "SwapFree:") name = "villagesql_host_swap_free_bytes";
    if (name != nullptr) out.gauge_i(name, static_cast<long long>(kb) * 1024);
  }
}

// The filesystem the datadir sits on, and the block device under it. The
// device is matched by major:minor in /proc/diskstats, which also works for a
// bind mount into a container; on a filesystem with no backing block device
// (overlay, tmpfs) the I/O counters are simply not emitted.
void emit_datadir(MetricBatch &out, const std::string &datadir) {
  struct statvfs vfs;
  if (statvfs(datadir.c_str(), &vfs) == 0) {
    out.gauge_i("villagesql_host_datadir_fs_size_bytes",
                static_cast<long long>(vfs.f_blocks) * static_cast<long long>(vfs.f_frsize));
    out.gauge_i("villagesql_host_datadir_fs_avail_bytes",
                static_cast<long long>(vfs.f_bavail) * static_cast<long long>(vfs.f_frsize));
  }

  struct stat st;
  if (stat(datadir.c_str(), &st) != 0) return;
  const unsigned want_major = major(st.st_dev);
  const unsigned want_minor = minor(st.st_dev);

  std::ifstream f("/proc/diskstats");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream in(line);
    unsigned dev_major = 0, dev_minor = 0;
    std::string device;
    unsigned long long reads, reads_merged, sectors_read, ms_reading, writes,
        writes_merged, sectors_written, ms_writing, in_flight, ms_io;
    if (!(in >> dev_major >> dev_minor >> device >> reads >> reads_merged >>
          sectors_read >> ms_reading >> writes >> writes_merged >>
          sectors_written >> ms_writing >> in_flight >> ms_io)) {
      continue;
    }
    if (dev_major != want_major || dev_minor != want_minor) continue;
    // /proc/diskstats sectors are always 512 bytes regardless of the device.
    out.counter_i("villagesql_host_datadir_disk_read_bytes_total",
                  static_cast<long long>(sectors_read) * 512, {{"device", device}});
    out.counter_i("villagesql_host_datadir_disk_written_bytes_total",
                  static_cast<long long>(sectors_written) * 512, {{"device", device}});
    out.counter_d("villagesql_host_datadir_disk_io_time_seconds_total",
                  static_cast<double>(ms_io) / 1000.0, {{"device", device}});
    return;
  }
}

// Regular files in cores_dir. A crash leaves one here (kernel.core_pattern
// points at it), and the restarted server reports it. Empty cores_dir means
// the operator has not set one up, so nothing is emitted; a set but unreadable
// directory is a misconfiguration and fails the collector rather than
// reporting a reassuring zero.
bool emit_cores(CollectorCtx &ctx) {
  const std::string &dir = ctx.cfg->cores_dir;
  if (dir.empty()) return true;
  DIR *d = opendir(dir.c_str());
  if (d == nullptr) {
    *ctx.err = "cannot open cores_dir " + dir;
    return false;
  }
  long long count = 0;
  while (struct dirent *entry = readdir(d)) {
    struct stat st;
    const std::string path = dir + "/" + entry->d_name;
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) ++count;
  }
  closedir(d);
  ctx.out->gauge_i("villagesql_core_files", count);
  return true;
}

}  // namespace
#endif

// The machine the server runs on, as far as the server process can see it
// from /proc. Enough to tell a leaking server from a starved host and to
// watch the data disk fill, without a second exporter on the box.
bool collect_host(CollectorCtx &ctx) {
#ifdef __linux__
  emit_cpu(*ctx.out);
  emit_meminfo(*ctx.out);

  std::string datadir;
  const bool ok = query_rows(ctx, "SELECT @@datadir", [&](const sq::SqlQuery::Row &row) {
    datadir = std::string(col_str(row, 0));
  });
  if (!ok) return false;
  if (!datadir.empty()) emit_datadir(*ctx.out, datadir);

  return emit_cores(ctx);
#else
  // No /proc: emit nothing rather than fail every scrape on macOS dev builds.
  (void)ctx;
  return true;
#endif
}

}  // namespace vsql_metrics
