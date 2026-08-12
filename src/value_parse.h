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

#ifndef VSQL_METRICS_VALUE_PARSE_H
#define VSQL_METRICS_VALUE_PARSE_H

#include <string>
#include <string_view>

namespace vsql_metrics {

// Lowercases and replaces every character outside [a-zA-Z0-9_] with '_', so an
// arbitrary status or variable key becomes a legal Prometheus metric-name
// component. Mirrors mysqld_exporter's validPrometheusName.
//
// Upstream applies this only to SHOW GLOBAL VARIABLES keys, because MySQL's own
// status variable names never contain anything but [A-Za-z0-9_]. VillageSQL
// extension status vars do -- they are registered as "<extension>.<var>" -- so
// this exporter applies it to status keys too.
std::string valid_prometheus_name(std::string_view raw);

// Returns the part of `name` after `prefix`, or an empty view if `name` does
// not start with `prefix`. An exact match also returns empty: every caller
// treats "no suffix" and "no match" the same way, since a bare prefix is not a
// status key either exporter emits.
inline std::string_view strip_prefix(std::string_view name,
                                     std::string_view prefix) {
  if (name.size() <= prefix.size() ||
      name.compare(0, prefix.size(), prefix) != 0) {
    return {};
  }
  return name.substr(prefix.size());
}

// Converts a SHOW GLOBAL STATUS / VARIABLES / REPLICA STATUS value to a float.
// Returns false when the value is not representable, in which case no metric
// should be emitted at all -- this is what drops the PEM-blob status variables
// (Rsa_public_key, Caching_sha2_password_rsa_public_key).
//
// Port of mysqld_exporter's parseStatus. Order is significant.
bool parse_status_value(std::string_view raw, double *out);

}  // namespace vsql_metrics

#endif  // VSQL_METRICS_VALUE_PARSE_H
