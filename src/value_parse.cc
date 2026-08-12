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

#include "value_parse.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vsql_metrics {
namespace {

char lower_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// ASCII case-insensitive compare against a NUL-terminated literal. std::tolower
// is locale-dependent, and these are protocol tokens, not text.
bool iequals(std::string_view value, const char *lowercase_literal) {
  std::size_t i = 0;
  for (; lowercase_literal[i] != '\0'; ++i) {
    if (i >= value.size() || lower_ascii(value[i]) != lowercase_literal[i]) {
      return false;
    }
  }
  return i == value.size();
}

// Index of a three-letter English month abbreviation, or -1.
int month_index(const char *abbrev) {
  static const char *kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; ++i) {
    if (std::strcmp(abbrev, kMonths[i]) == 0) return i;
  }
  return -1;
}

bool valid_time_of_day(int hour, int minute, int second) {
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 &&
         second <= 60;  // 60 allows a leap second
}

double to_unix_seconds(int year, int month_idx, int day, int hour, int minute,
                       int second) {
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month_idx;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  return static_cast<double>(timegm(&tm));
}

// Go layout "Jan _2 15:04:05 2006 MST", e.g. "Aug  5 20:44:09 2036 GMT".
//
// Parsed by hand rather than with strptime: glibc's %Z does not reliably
// consume a zone abbreviation, and %d will not match Go's "_2" space-padded
// day. sscanf's %d skips leading blanks, which handles the padding. The zone is
// read but treated as UTC -- MySQL emits these certificate timestamps in GMT.
bool parse_go_ansic_time(const std::string &str, double *out) {
  char month[8] = {0};
  char zone[16] = {0};
  int day = 0, hour = 0, minute = 0, second = 0, year = 0;
  int consumed = 0;
  if (std::sscanf(str.c_str(), "%7s %d %d:%d:%d %d %15s%n", month, &day, &hour,
                  &minute, &second, &year, zone, &consumed) != 7) {
    return false;
  }
  if (consumed != static_cast<int>(str.size())) return false;

  const int mon = month_index(month);
  if (mon < 0 || day < 1 || day > 31 || !valid_time_of_day(hour, minute, second)) {
    return false;
  }
  *out = to_unix_seconds(year, mon, day, hour, minute, second);
  return true;
}

// Go layout time.DateTime, "2006-01-02 15:04:05", e.g. Max_used_connections_time.
bool parse_datetime(const std::string &str, double *out) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int consumed = 0;
  if (std::sscanf(str.c_str(), "%d-%d-%d %d:%d:%d%n", &year, &month, &day, &hour,
                  &minute, &second, &consumed) != 6) {
    return false;
  }
  if (consumed != static_cast<int>(str.size())) return false;

  if (month < 1 || month > 12 || day < 1 || day > 31 ||
      !valid_time_of_day(hour, minute, second)) {
    return false;
  }
  *out = to_unix_seconds(year, month - 1, day, hour, minute, second);
  return true;
}

// strtod that requires the entire string to be consumed, so "3 threads" and
// "TLSv1.2,TLSv1.3" are rejected rather than silently truncated to a number.
bool parse_full_double(const std::string &str, double *out) {
  if (str.empty()) return false;
  errno = 0;
  char *end = nullptr;
  const double value = std::strtod(str.c_str(), &end);
  if (end != str.c_str() + str.size()) return false;
  if (errno == ERANGE) return false;
  *out = value;
  return true;
}

}  // namespace

std::string valid_prometheus_name(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    const char lowered = lower_ascii(c);
    const bool legal = (lowered >= 'a' && lowered <= 'z') ||
                       (lowered >= '0' && lowered <= '9') || lowered == '_';
    out += legal ? lowered : '_';
  }
  return out;
}

bool parse_status_value(std::string_view raw, double *out) {
  // Step 1: the fixed vocabulary, matched case-insensitively against the view
  // itself. This path runs about 1,200 times per scrape and almost always
  // falls through, so it does not allocate.
  for (const char *word : {"yes", "on", "primary"}) {
    if (iequals(raw, word)) {
      *out = 1.0;
      return true;
    }
  }
  for (const char *word :
       {"no", "off", "disabled", "connecting", "non-primary", "disconnected"}) {
    if (iequals(raw, word)) {
      *out = 0.0;
      return true;
    }
  }

  const std::string str(raw);

  // Steps 2 and 3: timestamps, parsed from the original (not lowercased) text.
  if (parse_go_ansic_time(str, out)) return true;
  if (parse_datetime(str, out)) return true;

  // Upstream has a further branch here guarded by the regexp `.+\.(\d+)$`,
  // intended to pull a log-file sequence number out of names like
  // "mysql-bin.000004". It uses Find rather than FindSubmatch, so the text it
  // hands to ParseFloat is the whole matched string, not the captured digits --
  // which means it only ever succeeds for plain decimals such as "123.456",
  // exactly what the fallback below already accepts. Reproducing it would add a
  // branch that can never change the result.
  return parse_full_double(str, out);
}

}  // namespace vsql_metrics
