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

// Prometheus text exposition format, rendered from the same MetricBatch the
// OTLP path serializes. The two formats have opposite hazards: OTLP's are
// encoding rules (quoted int64s, temporality, monotonicity), and this format's
// are textual -- a stray newline in a label value or a repeated # TYPE line
// makes a scraper reject the whole body.

#include "prom_text.h"

#include <cmath>
#include <cstdio>

namespace vsql_metrics {
namespace prom_text {
namespace {

// Escapes a label value: backslash, double quote and newline, per the
// exposition format. A carriage return has no escape in the spec and would
// terminate the line early, so it is dropped rather than emitted raw.
void append_label_value(std::string &out, const std::string &value) {
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      default: out += c; break;
    }
  }
}

void append_value(std::string &out, const MetricFamily &family,
                  const DataPoint &point) {
  if (family.value_kind == ValueKind::kInt) {
    out += std::to_string(point.i);
    return;
  }
  const double v = point.d;
  if (std::isnan(v)) {
    out += "NaN";
    return;
  }
  if (std::isinf(v)) {
    out += (v > 0) ? "+Inf" : "-Inf";
    return;
  }
  // %.17g round-trips a double exactly. The exposition format accepts Go's
  // float syntax, of which this is a subset.
  char buf[40];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  out.append(buf, static_cast<std::size_t>(n));
}

}  // namespace

std::string serialize(const MetricBatch &batch) {
  std::string out;
  out.reserve(batch.point_count() * 96);

  for (const MetricFamily &family : batch.families()) {
    if (family.points.empty()) continue;

    // Exactly one TYPE line per family, before its samples. Repeating a TYPE
    // for the same metric name is a parse error, which is why this is emitted
    // per family rather than per sample -- MetricBatch already groups the
    // points, so the grouping comes for free.
    //
    // The type is taken from how the value actually behaves, not from the
    // metric name. Several names carry a _total suffix that mysqld_exporter
    // gave them while typing them counter, even though the underlying value
    // rises and falls; those are grouped as gauges here and are reported as
    // gauges. The name is unchanged, so dashboards are unaffected.
    out += "# TYPE ";
    out += family.name;
    out += (family.kind == Kind::kSum) ? " counter\n" : " gauge\n";

    for (const DataPoint &point : family.points) {
      out += family.name;
      if (!point.labels.empty()) {
        out += '{';
        bool first = true;
        for (const Label &label : point.labels) {
          if (!first) out += ',';
          first = false;
          out.append(label.key.data(), label.key.size());
          out += "=\"";
          append_label_value(out, label.value);
          out += '"';
        }
        out += '}';
      }
      out += ' ';
      append_value(out, family, point);
      out += '\n';
    }
  }
  return out;
}

}  // namespace prom_text
}  // namespace vsql_metrics
