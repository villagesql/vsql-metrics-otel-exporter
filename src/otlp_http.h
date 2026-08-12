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

#ifndef VSQL_METRICS_OTLP_HTTP_H
#define VSQL_METRICS_OTLP_HTTP_H

#include <string>
#include <string_view>
#include <vector>

namespace vsql_metrics {
namespace otlp_http {

struct Response {
  bool ok = false;
  long http_status = 0;
  int curl_error = 0;  // CURLcode; 0 when the request itself completed
  std::string message;
};

// Called once before the first request. Safe to call repeatedly.
void global_init();

// Joins an OTLP base URL with a signal path, tolerating a trailing slash on the
// base ("http://host:4318/" + "/v1/metrics" must not become a double slash).
std::string join_url(std::string_view base, std::string_view path);

// Parses the otlp_headers sysvar: comma-separated Key=Value pairs, matching the
// OTEL_EXPORTER_OTLP_HEADERS convention. Returns false and sets *err on a
// malformed entry or on any CR/LF, which would otherwise allow header injection
// through a SET GLOBAL.
bool parse_headers(std::string_view spec, std::vector<std::string> *out,
                   std::string *err);

// POSTs a JSON body. Never throws; every failure is reported in Response.
Response post_json(const std::string &url, const std::string &body,
                   const std::vector<std::string> &headers, long timeout_secs);

}  // namespace otlp_http
}  // namespace vsql_metrics

#endif  // VSQL_METRICS_OTLP_HTTP_H
