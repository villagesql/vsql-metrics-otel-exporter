# VillageSQL Metrics OTel Exporter

Scrapes VillageSQL server metrics on a timer and pushes them to an OpenTelemetry collector as
OTLP/HTTP metrics — using [mysqld_exporter](https://github.com/prometheus/mysqld_exporter)'s
metric names and labels, so existing Grafana dashboards and alert rules keep working.

The point is that you do not run a separate exporter process. No sidecar, no second set of
database credentials, no extra network path — the server exports its own metrics.

## Contents

- [Quick start](#quick-start)
- [Building](#building)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [Configuration](#configuration)
- [Collectors](#collectors)
- [Metric reference](#metric-reference)
- [Security considerations](#security-considerations)
- [Known limitations](#known-limitations)
- [Testing](#testing)
- [Contributing](#contributing)
- [Reporting bugs and requesting features](#reporting-bugs-and-requesting-features)
- [Contact](#contact)
- [License](#license)

## Quick start

End to end, from a clean checkout to metrics arriving at a collector. Substitute your own
VillageSQL build directory throughout.

```bash
cd vsql-metrics-otel-exporter
export VillageSQL_BUILD_DIR=$HOME/build/villagesql
```

**1. Build.**

```bash
./build.sh                                    # -> build/vsql_metrics_otel_exporter.veb
```

**2. Start a collector to receive the metrics.** Any OpenTelemetry Collector with an OTLP/HTTP
receiver on `:4318` works; this one writes what it receives to a file so you can look at it.

```bash
mkdir -p otel-out && chmod 777 otel-out
docker run -d --name vsql-metrics-otel -p 4318:4318 \
  -v "$PWD/test/otel-collector-config.yaml:/etc/otelcol/config.yaml" \
  -v "$PWD/otel-out:/output" otel/opentelemetry-collector
```

**3. Run the tests** — optional, but this is the fastest confirmation that the build works
against your server. With the collector from step 2 running, the live conformance test runs too;
without it, that one test skips and the rest stay hermetic.

```bash
./test.sh
```

**4. Stage the extension and install it.**

```bash
cp build/vsql_metrics_otel_exporter.veb "$VillageSQL_BUILD_DIR/lib/veb/"
"$VillageSQL_BUILD_DIR/bin/mysql" -u root -p
```

```sql
-- Once per server: this extension uses preview capabilities. The server rejects a
-- plain SET GLOBAL here, so use SET PERSIST.
SET PERSIST vsql_allow_preview_extensions = ON;

INSTALL EXTENSION vsql_metrics_otel_exporter;

SET GLOBAL vsql_metrics_otel_exporter.otlp_endpoint = 'http://127.0.0.1:4318';
SET GLOBAL vsql_metrics_otel_exporter.export_interval_ms = 2000;  -- set BEFORE enabling
SET GLOBAL vsql_metrics_otel_exporter.enabled = ON;
```

**5. Confirm it is working.**

```sql
SHOW GLOBAL STATUS LIKE 'vsql_metrics_otel_exporter.%';
```

`exports_total` climbing with `export_errors_total` at 0 is what you want. Then look at what
actually arrived:

```bash
head -c 400 otel-out/received.json
```

**6. Turn on more collectors** whenever you like — they take effect on the next tick.

```sql
SET GLOBAL vsql_metrics_otel_exporter.collect_perf_schema_eventswaits = ON;
SET GLOBAL vsql_metrics_otel_exporter.collect_info_schema_innodb_metrics = ON;
```

> **Set `export_interval_ms` before enabling the worker.** It is read at the *end* of each tick,
> so changing it from an hour to a second takes up to an hour to apply. Toggling `enabled` off
> and on applies it immediately.

## Building

Prerequisites: a VillageSQL build directory, CMake 3.16+, a C++17 compiler, and libcurl
development headers.

**Debian/Ubuntu**

```bash
sudo apt-get install -y cmake g++ libcurl4-openssl-dev
```

**RHEL/Fedora**

```bash
sudo dnf install -y cmake gcc-c++ libcurl-devel
```

**macOS** — libcurl ships with the Xcode Command Line Tools.

```bash
xcode-select --install
brew install cmake
```

Then:

```bash
export VillageSQL_BUILD_DIR=$HOME/build/villagesql
./build.sh
```

This produces `build/vsql_metrics_otel_exporter.veb`.

## Installing

The exporter uses preview extension capabilities (a background worker and internal SQL), so the
server must allow them. The server rejects a plain `SET GLOBAL` for this variable — use
`SET PERSIST` so it survives restart:

```sql
SET PERSIST vsql_allow_preview_extensions = ON;
```

Copy the `.veb` into the server's `veb_dir`, then:

```sql
INSTALL EXTENSION vsql_metrics_otel_exporter;

SET GLOBAL vsql_metrics_otel_exporter.otlp_endpoint = 'http://collector:4318';
SET GLOBAL vsql_metrics_otel_exporter.enabled = ON;
```

`/v1/metrics` is appended to `otlp_endpoint`. That is the whole quick start — with the default
collector set, the server begins exporting roughly 1,000 data points every 10 seconds.

Check that it is working:

```sql
SHOW GLOBAL STATUS LIKE 'vsql_metrics_otel_exporter.%';
```

`exports_total` should be climbing and `export_errors_total` should be 0.

### A collector to point it at

Any OpenTelemetry Collector with an OTLP/HTTP receiver on `:4318` will do. For local testing,
`test/otel-collector-config.yaml` in this repo writes received metrics to a file:

```bash
docker run -d --name vsql-metrics-otel -p 4318:4318 \
  -v "$PWD/test/otel-collector-config.yaml:/etc/otelcol/config.yaml" \
  -v "$PWD/otel-out:/output" otel/opentelemetry-collector
```

To reach Prometheus, add a `prometheusremotewrite` exporter to the collector's metrics pipeline.
The metric names arrive already in mysqld_exporter's spelling, so nothing needs renaming.

## Uninstalling

```sql
SET GLOBAL vsql_metrics_otel_exporter.enabled = OFF;
UNINSTALL EXTENSION vsql_metrics_otel_exporter;
```

Uninstalling removes every system and status variable the extension registered, so nothing is
left behind in `SHOW VARIABLES` or `SHOW GLOBAL STATUS`. The extension defines no custom types
and no functions, so no table or index can depend on it — there is nothing to migrate first.

Then remove the staged package, and the test collector if you started one:

```bash
rm -f "$VillageSQL_BUILD_DIR/lib/veb/vsql_metrics_otel_exporter.veb"
docker rm -f vsql-metrics-otel && rm -rf otel-out
```

`vsql_allow_preview_extensions` can stay on; the server refuses to turn it off while any preview
extension is still installed, and it is harmless once none are.

## Configuration

All variables are prefixed `vsql_metrics_otel_exporter.`.

| Variable | Default | Range | Purpose |
|---|---|---|---|
| `enabled` | `OFF` | | Start and stop the background worker |
| `otlp_endpoint` | `http://localhost:4318` | | Collector base URL; empty stops sending without stopping scraping |
| `otlp_headers` | *(empty)* | | Extra HTTP headers, `Key=Value` comma separated |
| `service_name` | `villagesql` | | OTLP resource attribute `service.name` |
| `service_instance_id` | *(empty)* | | OTLP resource attribute `service.instance.id`; omitted when empty |
| `export_interval_ms` | `10000` | 1000–3600000 | Scrape and export interval |
| `http_timeout_secs` | `10` | 1–300 | Timeout for one export request |
| `max_datapoints_per_request` | `2000` | 100–1000000 | Split larger scrapes across several requests |
| `max_body_bytes` | `4194304` | 65536–134217728 | Size backstop for one request body |
| `processlist_min_time` | `0` | 0–86400 | Ignore threads younger than this, in seconds |
| `eventsstatements_limit` | `250` | 1–10000 | Maximum statement digests per scrape |
| `eventsstatements_timelimit` | `86400` | 1–604800 | Only report digests seen within this many seconds |
| `eventsstatements_digest_text_limit` | `120` | 1–4096 | Truncate `digest_text` label values |

Plus one `collect_*` switch per collector — see below.

**Interval changes take effect after the current sleep.** The worker reads the interval at the
end of each tick, so lowering it from an hour to a second takes up to an hour to apply. Toggling
`enabled` off and on applies it immediately.

### Sending to a hosted backend

`otlp_headers` carries whatever the backend needs for authentication:

```sql
SET GLOBAL vsql_metrics_otel_exporter.otlp_endpoint = 'https://otlp.example.com';
SET GLOBAL vsql_metrics_otel_exporter.otlp_headers  = 'Authorization=Bearer TOKEN, X-Scope-OrgID=42';
```

Read [Security considerations](#security-considerations) before doing this.

## Collectors

Each collector corresponds to a mysqld_exporter `--collect.*` flag, and the defaults match
mysqld_exporter's: the same five on, the rest off. Variable names cannot contain a dot, so the
flag name is transliterated with underscores.

| mysqld_exporter flag | Variable | Default |
|---|---|---|
| `--collect.global_status` | `collect_global_status` | **ON** |
| `--collect.global_variables` | `collect_global_variables` | **ON** |
| `--collect.slave_status` | `collect_slave_status` | **ON** |
| `--collect.info_schema.innodb_cmp` | `collect_info_schema_innodb_cmp` | **ON** |
| `--collect.info_schema.innodb_cmpmem` | `collect_info_schema_innodb_cmpmem` | **ON** |
| `--collect.info_schema.innodb_metrics` | `collect_info_schema_innodb_metrics` | OFF |
| `--collect.info_schema.processlist` | `collect_info_schema_processlist` | OFF |
| `--collect.info_schema.tables` | `collect_info_schema_tables` | OFF |
| `--collect.perf_schema.eventsstatements` | `collect_perf_schema_eventsstatements` | OFF |
| `--collect.perf_schema.eventsstatementssum` | `collect_perf_schema_stmt_sum` | OFF |
| `--collect.perf_schema.eventswaits` | `collect_perf_schema_eventswaits` | OFF |
| `--collect.perf_schema.tableiowaits` | `collect_perf_schema_tableiowaits` | OFF |
| `--collect.perf_schema.indexiowaits` | `collect_perf_schema_indexiowaits` | OFF |
| `--collect.perf_schema.file_events` | `collect_perf_schema_file_events` | OFF |
| `--collect.perf_schema.tablelocks` | `collect_perf_schema_tablelocks` | OFF |
| `--collect.engine_innodb_status` | `collect_engine_innodb_status` | OFF |
| `--collect.binlog_size` | `collect_binlog_size` | OFF |

`collect_perf_schema_stmt_sum` is the one name that is not a direct transliteration: the full
form is 39 characters, and the server silently truncates a qualified variable name at 64.

**`collect_info_schema_tables` is high cardinality** — it emits five series per table. On a
server with tens of thousands of tables, leave it off or expect a correspondingly large export.

A collector that fails does not stop the others: its failure is counted, reported in
`mysql_exporter_collector_success`, logged to the error log, and the rest of the scrape is
exported normally.

## Metric reference

Names and labels match mysqld_exporter. A few worth knowing:

| Metric | Labels | Notes |
|---|---|---|
| `mysql_up` | — | 1 when the scrape could open a session, 0 otherwise |
| `mysql_exporter_collector_success` | `collector` | Per-collector health, 0 when that collector failed |
| `mysql_exporter_collector_duration_seconds` | `collector` | Per-collector scrape time |
| `mysql_global_status_commands_total` | `command` | From `Com_*` |
| `mysql_global_status_handlers_total` | `handler` | From `Handler_*` |
| `mysql_global_status_innodb_row_ops_total` | `operation` | From `Innodb_rows_*` |
| `mysql_global_status_buffer_pool_pages` | `state` | `dirty` is reported separately, as it overlaps the other states |
| `mysql_global_status_<name>` | — | Everything else in `SHOW GLOBAL STATUS`, lowercased |
| `mysql_global_variables_<name>` | — | Numeric-valued entries of `SHOW GLOBAL VARIABLES` |
| `mysql_version_info` | `version`, `version_comment`, `innodb_version` | Always 1 |

Counters are emitted as cumulative monotonic OTLP sums; everything else is a gauge.

**Status values that are not numbers produce no metric at all.** `ON`/`OFF` become 1/0,
certificate and datetime values become Unix timestamps, and anything else — notably the
`Rsa_public_key` PEM blobs — is skipped rather than exported as zero. This matches
mysqld_exporter's behaviour.

**Extension status variables are sanitised.** VillageSQL names them `<extension>.<var>`, and a
dot is not legal in a metric name, so `vsql_stat_otel.events_captured` arrives as
`mysql_global_status_vsql_stat_otel_events_captured`. mysqld_exporter has no equivalent rule
because MySQL's own status variables never contain a dot.

**Counter resets are not detected.** `startTimeUnixNano` is the process start time, so a
`FLUSH STATUS` resets the underlying counters without the exporter marking a reset, and `rate()`
sees a dip. mysqld_exporter behaves the same way.

## Security considerations

**`otlp_headers` holds credentials in a system variable.** Anything you put there — a bearer
token, an API key — is visible to any user who can run `SHOW VARIABLES`, appears in
`performance_schema.global_variables`, and is written to `mysqld-auto.cnf` in plain text if you
`SET PERSIST` it. Treat it as a shared secret: grant it the same care as a replication password,
prefer a collector on localhost or a private network over a hosted endpoint with a token, and
remember that `SET GLOBAL` statements can be captured by the query log and by this server's own
statement telemetry.

Header values are rejected if they contain CR or LF, so a header cannot be used to inject a
second header or a request body.

**The exporter sends server metadata off the box.** `SHOW GLOBAL VARIABLES` includes paths,
version strings, and configuration detail; `info_schema.tables` includes schema and table names;
`perf_schema.eventsstatements` includes normalised query text as a label. Anyone with access to
your metrics backend can read all of it. The last of those is off by default, and
`eventsstatements_digest_text_limit` bounds how much query text leaves the server.

**Use TLS for anything beyond localhost.** An `https://` endpoint is verified against the system
CA bundle. Plain `http://` sends everything above in clear text.

**Privileges.** The background worker runs its queries with full privileges, which is what lets
it read `SHOW ENGINE INNODB STATUS` and the complete process list. It only ever reads; it issues
no DDL or DML. Installing the extension is therefore equivalent to granting your metrics
pipeline read access to server state.

## Known limitations

- **Four mysqld_exporter collectors cannot exist on this server.**
  `info_schema.query_response_time` (one of mysqld_exporter's defaults), `info_schema.userstats`,
  `info_schema.clientstats` and `info_schema.tablestats` depend on Percona/MariaDB `userstat`
  tables that this build does not have. `heartbeat` needs an external `pt-heartbeat`. Panels fed
  by those metrics will be empty. Merging the Percona enhancements that provide User Statistics
  and global query response-time statistics is tracked upstream in
  [villagesql-server#622](https://github.com/villagesql/villagesql-server/issues/622) — 👍 it if
  you want these collectors.
- **Push, not pull.** No `/metrics` endpoint; a collector must sit between this and Prometheus.
- **No retry and no compression.** A failed export is dropped. Cumulative counters self-heal on
  the next tick; gauges lose a sample. Bodies are not gzipped.
- **Preview APIs.** Requires `vsql_allow_preview_extensions = ON`. These capabilities may change
  between VillageSQL releases, in API and ABI, with no deprecation window. A path to stabilizing
  them is tracked in
  [villagesql-server#623](https://github.com/villagesql/villagesql-server/issues/623).
- **Replication metrics are mapped positionally.** The extension API exposes no column names, so
  `SHOW REPLICA STATUS` columns are matched against a transcribed list. If a future server
  version changes that list, the collector reports a failure rather than guessing.
- **No fractional-valued settings.** Extension system variables cannot be floating point, so
  every knob is an integer.
- **The last error message is not readable from SQL.** Extension status variables can only be
  numeric. `last_export_http_status`, `last_export_curl_errno` and `last_scrape_error_collector`
  classify the failure; the text goes to the server error log.
- **No export on shutdown.** At most one interval is skipped; because the metrics are cumulative
  counters read fresh from the server, the next export after restarting carries the full value.

The API gaps behind several of these are tracked upstream — see
[Reporting bugs and requesting features](#reporting-bugs-and-requesting-features) if one is
blocking you, as demand is what gets them prioritised.

## Testing

See [TESTING.md](TESTING.md).

## Contributing

See the [VillageSQL Contributing Guide](https://github.com/villagesql/villagesql-server/blob/main/CONTRIBUTING.md).

## Reporting bugs and requesting features

Please open an issue on this repository's GitHub Issues page. For gaps in the VillageSQL
extension API itself, open an issue on
[villagesql-server](https://github.com/villagesql/villagesql-server/issues).

## Contact

- Discord: https://discord.gg/KSr6whd3Fr
- GitHub Issues

## License

GPL v2 — see [LICENSE](LICENSE).
