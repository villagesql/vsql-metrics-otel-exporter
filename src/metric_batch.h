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

#ifndef VSQL_METRICS_METRIC_BATCH_H
#define VSQL_METRICS_METRIC_BATCH_H

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vsql_metrics {

struct Label {
  // Label keys are always compile-time constants -- no collector derives one
  // from a row -- so this borrows rather than owning. Values do come from rows
  // and are owned.
  std::string_view key;
  std::string value;
};

// Sum always means cumulative and monotonic here: every counter this exporter
// emits is a server-lifetime running total read fresh from the server.
enum class Kind { kGauge, kSum };

// Chosen per metric name, never per value. A series that alternates between
// asInt and asDouble across scrapes splits into two series downstream.
enum class ValueKind { kInt, kDouble };

struct DataPoint {
  std::vector<Label> labels;
  long long i = 0;
  double d = 0.0;
};

struct MetricFamily {
  std::string name;
  Kind kind = Kind::kGauge;
  ValueKind value_kind = ValueKind::kDouble;
  std::vector<DataPoint> points;
};

// Accumulates one scrape's metrics, grouping data points under a shared family
// name. OTLP wants one metrics[] entry per name carrying all of its data
// points, and mysql_global_status_commands_total alone has ~169.
class MetricBatch {
 public:
  void gauge_i(std::string_view name, long long v, std::vector<Label> labels = {}) {
    add(name, Kind::kGauge, ValueKind::kInt, v, 0.0, std::move(labels));
  }

  void gauge_d(std::string_view name, double v, std::vector<Label> labels = {}) {
    add(name, Kind::kGauge, ValueKind::kDouble, 0, v, std::move(labels));
  }

  void counter_i(std::string_view name, long long v, std::vector<Label> labels = {}) {
    add(name, Kind::kSum, ValueKind::kInt, v, 0.0, std::move(labels));
  }

  void counter_d(std::string_view name, double v, std::vector<Label> labels = {}) {
    add(name, Kind::kSum, ValueKind::kDouble, 0, v, std::move(labels));
  }

  const std::vector<MetricFamily> &families() const { return families_; }
  std::size_t point_count() const { return point_count_; }

  // Number of points whose kind or value kind disagreed with the family they
  // landed in. Always zero in practice -- each metric name is emitted from
  // exactly one place -- but a collision would serialize the wrong union
  // member, so it is counted rather than left to be noticed in a dashboard.
  std::size_t kind_conflicts() const { return kind_conflicts_; }

 private:
  void add(std::string_view name, Kind kind, ValueKind value_kind, long long i,
           double d, std::vector<Label> labels) {
    auto it = index_.find(name);
    if (it == index_.end()) {
      MetricFamily fam;
      fam.name = std::string(name);
      fam.kind = kind;
      fam.value_kind = value_kind;
      families_.push_back(std::move(fam));
      it = index_.emplace(std::string(name), families_.size() - 1).first;
    } else if (families_[it->second].kind != kind ||
               families_[it->second].value_kind != value_kind) {
      ++kind_conflicts_;
    }

    DataPoint point;
    point.labels = std::move(labels);
    point.i = i;
    point.d = d;
    // Indices, not pointers: push_back above may reallocate families_.
    families_[it->second].points.push_back(std::move(point));
    ++point_count_;
  }

  std::vector<MetricFamily> families_;
  // std::less<> makes this transparent, so looking a name up does not have to
  // materialize a std::string first. Around 3,500 points per scrape land in
  // ~60 families, so all but a handful of these lookups are hits on a name that
  // already exists -- allocating for each would be ~3,400 wasted round trips
  // through the allocator. An unordered_map would need C++20 for the same
  // trick; at 60 entries the tree is not the cost that matters.
  std::map<std::string, std::size_t, std::less<>> index_;
  std::size_t point_count_ = 0;
  std::size_t kind_conflicts_ = 0;
};

}  // namespace vsql_metrics

#endif  // VSQL_METRICS_METRIC_BATCH_H
