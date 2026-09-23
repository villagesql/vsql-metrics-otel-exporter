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

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace vsql_metrics {

// Keep crash evidence available after a server restart without collecting
// host resources here. An inaccessible configured directory is not a zero.
bool collect_core_files(CollectorCtx &ctx) {
#ifndef _WIN32
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
#else
  (void)ctx;
  return true;
#endif
}

}  // namespace vsql_metrics
