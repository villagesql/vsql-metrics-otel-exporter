# AGENTS.md

Guidance for AI coding assistants working in this repository.

## What this is

`vsql_metrics_otel_exporter` is a VillageSQL extension that scrapes server metrics on a timer
and pushes them to an OpenTelemetry collector as OTLP/HTTP metrics.

The product goal is **drop-in compatibility with
[mysqld_exporter](https://github.com/prometheus/mysqld_exporter)**: metric names and label sets
match it byte for byte, so existing Grafana dashboards and alert rules keep working once an
OTLP collector translates to Prometheus. That constraint dominates almost every design decision
here. If a metric name looks wrong in isolation, check it against
[mysqld_exporter's `collector/` source](https://github.com/prometheus/mysqld_exporter/tree/main/collector)
before changing it — several are counterintuitive on purpose because upstream is, and matching
upstream is the point.

Repo directory is hyphenated (`vsql-metrics-otel-exporter`); the install name is underscored
(`vsql_metrics_otel_exporter`) because it becomes the prefix of every system and status variable,
and a dot-or-hyphen in that position would need backticks in every SQL statement.

## Layout

```
src/
  extension.cc          entry point: sysvar/status-var tables, capabilities, worker registration
  scrape.h/.cc          collector registry, per-tick driver, timing, error accounting
  collector.h           Collector/CollectorCtx types, safe column accessors, query_rows
  metric_batch.h        MetricFamily / DataPoint / Label accumulation for one scrape
  otlp_json.h/.cc       MetricBatch -> chunked OTLP protobuf-JSON bodies. No I/O.
  otlp_http.h/.cc       libcurl POST and header parsing. No JSON knowledge.
  value_parse.h/.cc     mysqld_exporter's parseStatus port, name sanitization
  collectors/*.cc       one file per upstream mysqld_exporter collector family
mysql-test/t, mysql-test/r   MTR suite and recorded results
```

## Build and test

```bash
export VillageSQL_BUILD_DIR=/path/to/villagesql/build
./build.sh            # produces build/vsql_metrics_otel_exporter.veb
./test.sh             # build + run the MTR suite
RECORD=1 ./test.sh    # regenerate mysql-test/r/*.result
```

Never hand-write a `.result` file — always regenerate with `RECORD=1` and read the diff.

`test.sh` auto-detects an OpenTelemetry Collector on `http://127.0.0.1:4318`. If one is running,
`metrics_live.test` un-skips and validates the OTLP body against a real receiver; otherwise it
skips and the suite stays hermetic. Starting one is worth it before any change to
`otlp_json.cc` — it is the only check that a malformed body would actually be rejected.

## Constraints that are not obvious from the code

These each have a specific failure mode. Changing code near them without knowing them will
produce something that compiles and is wrong.

1. **The SDK exposes no column names**, only values by index. Collectors that cannot write their
   own SELECT list (`SHOW REPLICA STATUS`) map columns positionally against a transcribed list
   and guard on the column count. Do not remove those guards; a silent column shift reports
   wrong numbers indefinitely as a successful scrape.
2. **Never dereference the `char**` behind a string system variable.** The server frees the old
   string on `SET GLOBAL`, so the pointer can be freed under the worker. Read strings through
   `StringReader` (see `scrape.h`), which goes via the server's own locked reader.
3. **Integer sysvar and status-var globals must be `long long`, not `int64_t`.** `int64_t` is
   `long` on LP64 Linux; it compiles on macOS and fails to bind here.
4. **`sv::make_double` fails at registration** — MySQL's component variable service has no
   `PLUGIN_VAR_DOUBLE`, and a failed registration rolls back every variable before it. All
   numeric knobs are integers.
5. **No SQL is possible during `VEF_WAKEUP_ENABLE`** — the thread handle is null and the callback
   runs on the `SET GLOBAL` caller's thread.
6. **`sleep_ms == 0` means "keep the current interval", not "wake immediately."** The typed
   constructor leaves it 0, whose server-side default is 24 hours, so the worker must return a
   non-zero value from ENABLE or it never ticks.
7. **`column_str()` is valid only until the next row.** Copy before advancing. A SQL NULL is
   distinguishable from an empty string only by `data() == nullptr`; that is what the
   `col_str`/`col_int`/`col_real` helpers in `collector.h` exist for, along with bounds checking
   the SDK does not do.
8. **Row handlers run behind a C ABI boundary.** `query_rows` wraps them so an exception cannot
   unwind into the server. Keep it that way.
9. **`SHOW GLOBAL STATUS`, not `performance_schema.global_status`.** The view omits every `Com_*`
   counter — 169 of 505 rows on a stock server.
10. **The protobuf-JSON encoding rules in `otlp_json.cc` are load-bearing**: 64-bit integers
    (`asInt`, `timeUnixNano`, `startTimeUnixNano`) are quoted strings; doubles and enums are
    bare. Getting one backwards is accepted by some receivers and rejected by others. The test
    suite asserts both directions.

## Adding a collector

1. Add an enum entry to `CollectorIdx` and a matching row to `kCollectors[]` in `scrape.cc`,
   keeping the two in the same order — `static_assert`s in `scrape.cc` check the correspondence.
2. Declare the function in `collector.h`, implement it in the appropriate `collectors/*.cc`.
3. Add a `sv::make_bool` entry in `extension.cc` using `collect_name/flag/default(idx)`, so the
   SQL name and default come from the registry rather than being retyped.
4. Verify every metric name and label against mysqld_exporter's source before recording results.
   Do not reconstruct them from memory: seven names written that way during initial development
   turned out to be wrong, three of them in cases that felt certain.
5. Add the source file to `CMakeLists.txt` and extend the MTR suite.

## Conventions

- C++17. Lowercase-with-underscores for files and functions.
- GPL-2.0 header on every `.cc`, `.h`, and `CMakeLists.txt`.
- No `using namespace` at file scope; namespace aliases are fine.
- Comments explain why, especially where the reason is an external constraint. Do not add
  comments that restate the code.
