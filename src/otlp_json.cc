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

// OTLP/HTTP+JSON serialization for metrics. This file owns every place the
// protobuf JSON mapping is easy to get wrong:
//
//   - 64-bit integers (timeUnixNano, startTimeUnixNano, asInt) are QUOTED
//     strings. A bare number is silently a different value to some receivers
//     and a hard 400 from others.
//   - doubles (asDouble) are BARE numbers; non-finite doubles become the quoted
//     strings "NaN" / "Infinity" / "-Infinity".
//   - proto enums (aggregationTemporality) are BARE numbers.
//   - booleans (isMonotonic) are bare.
//
// The MTR suite asserts each of these directly against a captured body.

#include "otlp_json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

namespace vsql_metrics {
namespace otlp_json {
namespace {

// OTLP AggregationTemporality: 2 == CUMULATIVE. Emitted as the integer rather
// than the canonical enum name; every OTLP receiver accepts it and it is
// shorter on a body that carries thousands of metrics.
constexpr int kCumulative = 2;

// Tracks the comma between elements so no writer has to know whether it is
// first. The reference implementation this exporter borrows from passes a
// `first` flag to only one of its four element writers, which works there only
// because its arrays always start with the same type; metric attribute arrays
// do not, so the separator is made structural here instead.
class JsonArray {
 public:
  explicit JsonArray(std::string &out) : out_(out) { out_ += '['; }
  ~JsonArray() { out_ += ']'; }

  JsonArray(const JsonArray &) = delete;
  JsonArray &operator=(const JsonArray &) = delete;

  void separator() {
    if (!first_) out_ += ',';
    first_ = false;
  }

 private:
  std::string &out_;
  bool first_ = true;
};

void append_json_string(std::string &out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

// 64-bit integer fields are quoted in the protobuf JSON mapping. Both the
// unsigned timestamps and the signed asInt values go through here so neither
// can be emitted bare by accident.
template <typename Int>
void append_quoted_int(std::string &out, Int v) {
  out += '"';
  out += std::to_string(v);
  out += '"';
}

void append_double(std::string &out, double v) {
  if (std::isnan(v)) {
    out += "\"NaN\"";
    return;
  }
  if (std::isinf(v)) {
    out += (v > 0) ? "\"Infinity\"" : "\"-Infinity\"";
    return;
  }
  // %.17g round-trips an IEEE double exactly. %.6f, which the logs-side
  // reference uses, loses both large counter values and the small
  // picosecond-derived durations this exporter emits.
  char buf[40];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  out.append(buf, static_cast<std::size_t>(n));
}

void append_string_attr(std::string &out, JsonArray &arr, std::string_view key,
                        std::string_view value) {
  arr.separator();
  out += "{\"key\":";
  append_json_string(out, key);
  out += ",\"value\":{\"stringValue\":";
  append_json_string(out, value);
  out += "}}";
}

void append_resource_and_scope(std::string &out, const Options &opt) {
  out += "{\"resourceMetrics\":[{\"resource\":{\"attributes\":";
  {
    JsonArray attrs(out);
    append_string_attr(out, attrs, "service.name", opt.service_name);
    if (!opt.service_instance_id.empty()) {
      append_string_attr(out, attrs, "service.instance.id",
                         opt.service_instance_id);
    }
    append_string_attr(out, attrs, "db.system", "mysql");
  }
  out += "},\"scopeMetrics\":[{\"scope\":{\"name\":";
  append_json_string(out, opt.scope_name);
  out += ",\"version\":";
  append_json_string(out, opt.scope_version);
  out += "},\"metrics\":[";
}

void append_envelope_close(std::string &out) { out += "]}]}]}"; }

void append_data_point(std::string &out, const MetricFamily &fam,
                       const DataPoint &point, std::uint64_t time_unix_nano,
                       std::uint64_t start_time_unix_nano) {
  out += '{';
  if (fam.kind == Kind::kSum) {
    out += "\"startTimeUnixNano\":";
    append_quoted_int(out, start_time_unix_nano);
    out += ',';
  }
  out += "\"timeUnixNano\":";
  append_quoted_int(out, time_unix_nano);

  if (fam.value_kind == ValueKind::kInt) {
    out += ",\"asInt\":";
    append_quoted_int(out, point.i);
  } else {
    out += ",\"asDouble\":";
    append_double(out, point.d);
  }

  if (!point.labels.empty()) {
    out += ",\"attributes\":";
    JsonArray attrs(out);
    for (const Label &label : point.labels) {
      append_string_attr(out, attrs, label.key, label.value);
    }
  }
  out += '}';
}

// Emits one metrics[] entry carrying points [begin, begin+count) of `fam`.
void append_family_slice(std::string &out, const MetricFamily &fam,
                         std::size_t begin, std::size_t count,
                         std::uint64_t time_unix_nano,
                         std::uint64_t start_time_unix_nano) {
  out += "{\"name\":";
  append_json_string(out, fam.name);
  if (fam.kind == Kind::kSum) {
    out += ",\"sum\":{\"aggregationTemporality\":";
    out += std::to_string(kCumulative);
    out += ",\"isMonotonic\":true,\"dataPoints\":";
  } else {
    out += ",\"gauge\":{\"dataPoints\":";
  }
  {
    JsonArray points(out);
    for (std::size_t i = 0; i < count; ++i) {
      points.separator();
      append_data_point(out, fam, fam.points[begin + i], time_unix_nano,
                        start_time_unix_nano);
    }
  }
  out += "}}";
}

}  // namespace

std::vector<std::string> serialize(const MetricBatch &batch,
                                   std::uint64_t time_unix_nano,
                                   std::uint64_t start_time_unix_nano,
                                   const Options &opt) {
  std::vector<std::string> bodies;
  if (batch.point_count() == 0) return bodies;

  const std::size_t max_points = (opt.max_datapoints > 0) ? opt.max_datapoints : 1;

  std::string body;
  std::size_t points_in_body = 0;
  bool body_open = false;
  bool first_family_in_body = true;

  // Bounded by what the batch actually holds, not by the configured ceiling:
  // max_datapoints_per_request may legitimately be set to a million, and
  // reserving 256 MB per body to hold a few thousand points is not a plan.
  const std::size_t reserve_points = std::min(max_points, batch.point_count());
  const std::size_t reserve_bytes =
      std::min(reserve_points * 256 + 512, opt.max_body_bytes);

  auto open_body = [&]() {
    // The buffer is moved out when a body is closed, so this both clears it and
    // re-establishes capacity for the next one.
    body.clear();
    body.reserve(reserve_bytes);
    append_resource_and_scope(body, opt);
    points_in_body = 0;
    first_family_in_body = true;
    body_open = true;
  };

  auto close_body = [&]() {
    append_envelope_close(body);
    bodies.push_back(std::move(body));
    body_open = false;
  };

  for (const MetricFamily &fam : batch.families()) {
    std::size_t emitted = 0;
    while (emitted < fam.points.size()) {
      if (!body_open) open_body();

      // max_points >= 1 and a body is closed the moment it reaches the limit,
      // so there is always room for at least one point here.
      const std::size_t room = max_points - points_in_body;
      const std::size_t count = std::min(room, fam.points.size() - emitted);

      if (!first_family_in_body) body += ',';
      first_family_in_body = false;
      append_family_slice(body, fam, emitted, count, time_unix_nano,
                          start_time_unix_nano);

      emitted += count;
      points_in_body += count;

      if (points_in_body >= max_points || body.size() >= opt.max_body_bytes) {
        close_body();
      }
    }
  }
  if (body_open) close_body();

  return bodies;
}

}  // namespace otlp_json
}  // namespace vsql_metrics
