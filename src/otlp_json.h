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

#ifndef VSQL_METRICS_OTLP_JSON_H
#define VSQL_METRICS_OTLP_JSON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "metric_batch.h"

namespace vsql_metrics {
namespace otlp_json {

struct Options {
  std::string service_name;
  std::string service_instance_id;
  std::string scope_name;
  std::string scope_version;
  // Primary size limit. A family's points are split across bodies when it alone
  // exceeds this; repeating a metric name across requests is legal OTLP.
  std::size_t max_datapoints = 2000;
  // Backstop, checked after each family slice is appended, so a body may
  // overshoot by at most one slice.
  std::size_t max_body_bytes = 4u * 1024u * 1024u;
};

// Renders a batch as one or more complete OTLP/HTTP+JSON request bodies. Every
// body is an independent resourceMetrics envelope carrying identical resource
// attributes and the same time_unix_nano, so the chunks describe one logical
// scrape. Returns empty when the batch has no data points.
//
// start_time_unix_nano is emitted on sum data points only; gauges do not carry
// one.
std::vector<std::string> serialize(const MetricBatch &batch,
                                   std::uint64_t time_unix_nano,
                                   std::uint64_t start_time_unix_nano,
                                   const Options &opt);

}  // namespace otlp_json
}  // namespace vsql_metrics

#endif  // VSQL_METRICS_OTLP_JSON_H
