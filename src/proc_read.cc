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

#include "proc_read.h"

#ifdef __linux__

#include <dirent.h>
#include <sys/resource.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace vsql_metrics {

namespace {

// VmRSS in /proc/self/status is "VmRSS:      1454560 kB" -- the value is
// always in kB regardless of size.
bool read_rss_bytes(long long &rss_bytes, std::string *err) {
  std::ifstream status("/proc/self/status");
  if (!status.is_open()) {
    *err = "cannot open /proc/self/status";
    return false;
  }
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) != 0) continue;
    rss_bytes = std::atoll(line.c_str() + 6) * 1024;
    return true;
  }
  *err = "VmRSS not found in /proc/self/status";
  return false;
}

bool count_open_fds(long long &open_fds, std::string *err) {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    *err = "cannot open /proc/self/fd";
    return false;
  }
  long long count = 0;
  while (const dirent *entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    ++count;
  }
  closedir(dir);
  // The directory stream itself held one fd while we counted.
  open_fds = count - 1;
  return true;
}

}  // namespace

bool read_proc_self(ProcSelf &out, std::string *err) {
  if (!read_rss_bytes(out.rss_bytes, err)) return false;
  if (!count_open_fds(out.open_fds, err)) return false;

  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    *err = "getrlimit(RLIMIT_NOFILE) failed";
    return false;
  }
  out.max_fds = static_cast<long long>(limit.rlim_cur);
  return true;
}

}  // namespace vsql_metrics

#else  // __linux__

namespace vsql_metrics {

bool read_proc_self(ProcSelf &, std::string *err) {
  *err = "process metrics require /proc (Linux)";
  return false;
}

}  // namespace vsql_metrics

#endif  // __linux__
