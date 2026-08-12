# Testing

The suite runs under the MySQL Test Runner (MTR) against a server that MTR starts itself. Do not
start a server by hand.

## Prerequisites

| Requirement | Notes |
|---|---|
| `VillageSQL_BUILD_DIR` | Path to your VillageSQL build directory. Required. |
| libcurl headers | `libcurl4-openssl-dev` on Debian/Ubuntu, `libcurl-devel` on RHEL/Fedora, bundled with the Xcode Command Line Tools on macOS. |
| Python 3 | Several tests write a small HTTP receiver and run it in the background. |
| Docker *(optional)* | Only for the live conformance test. |

The suite enables preview capabilities itself via `mysql-test/t/suite.opt`, so no server-side
setup is needed.

## Running

```bash
export VillageSQL_BUILD_DIR=$HOME/build/villagesql

./test.sh            # build the VEB and run the suite
RECORD=1 ./test.sh   # regenerate the expected results
```

`test.sh` builds the extension, stages the `.veb` into a throwaway directory, and points MTR at
it with `--mysqld=--veb-dir`. Nothing is installed into your build tree.

To run one test:

```bash
cd "$VillageSQL_BUILD_DIR"
perl ./mysql-test/mysql-test-run.pl \
  --suite=/path/to/vsql-metrics-otel-exporter/mysql-test \
  --parallel=1 --nounit-tests \
  --mysqld=--veb-dir=/tmp/some-dir \
  metrics_otlp
```

## Regenerating expected results

Never edit a `.result` file by hand. Run `RECORD=1 ./test.sh` and read the diff — a change you
did not intend is the signal the suite exists to give you. Recorded output is machine-specific
in one respect only: ports are masked with `--replace_result`, so a recorded file should contain
no paths, no timings and no live metric values.

## Test files

| File | What it covers |
|---|---|
| `metrics_basic.test` | The configuration and health-counter surface: every variable and its default, every status counter, and that installing then uninstalling leaves the server with no residue. No network. |
| `metrics_otlp.test` | The exported body. Envelope structure, the metric names that dashboards depend on, label wiring, cumulative-sum framing, and the protobuf-JSON encoding rules — including that 64-bit integers are quoted and doubles are not. Also asserts that non-numeric status values produce no metric and that extension status names are sanitised. |
| `metrics_collectors.test` | That a `collect_*` variable actually controls its collector, by capturing two exports under different service names and comparing what each contains. |
| `metrics_errors.test` | Behaviour when things go wrong. A malformed `otlp_headers` list is rejected before any request is put on the wire; an unreachable collector is classified as a transport error with no HTTP status; an empty endpoint parks the exporter without counting an error; scraping is unaffected throughout; and — the point of the test — the worker keeps running through all of it. |
| `metrics_chunking.test` | That a scrape too large for one request is split into exactly as many complete, independently valid envelopes as the data-point count requires. |
| `metrics_live.test` | Conformance against a real OpenTelemetry Collector. Skipped unless one is running. |

## Why the assertions look the way they do

Metric values change on every scrape, so nothing in the suite compares a reading. Each assertion
either greps for a fixed string that appears a known number of times, or asks the server a
yes/no question that prints `1`. That is also why the tests count metric *names* — which appear
once per family — and never data points.

Where a test needs to inspect an exported body, it writes a small Python HTTP receiver, runs it
in the background, and has it keep only the body carrying that test's marker. The receiver
always answers 200, so a scrape landing mid-assertion cannot register a spurious export error,
and it stops itself on a deadline because MTR does not reap background children.

Tests set `export_interval_ms` **before** enabling the worker: the interval is read at the end of
each tick, so setting it afterwards would not apply until the current sleep expired.

## The live test

`metrics_live.test` is the only test that proves the OTLP body is actually *conformant* rather
than merely the right shape. The in-test receiver accepts anything; a real collector rejects a
malformed body with a 4xx, so `export_errors_total = 0` against it is the real assertion. It also
reads the metrics back out of the collector's own file exporter, confirming they survived its
pipeline.

Start a collector and it runs automatically:

```bash
docker run -d --name vsql-metrics-otel -p 4318:4318 \
  -v "$PWD/test/otel-collector-config.yaml:/etc/otelcol/config.yaml" \
  -v "$PWD/otel-out:/output" otel/opentelemetry-collector

./test.sh   # detects the collector on :4318 and un-skips the live test
```

`test.sh` probes `http://127.0.0.1:4318/v1/metrics` and sets `VSQL_METRICS_OTEL_ENDPOINT` and
`VSQL_METRICS_OTEL_OUTFILE` when it answers; otherwise the test skips and the suite stays
hermetic. Point it elsewhere by exporting those two variables yourself.

Run it after any change to `src/otlp_json.cc`. That file is where an encoding mistake would be
invisible to every other test.

## Manual checks

```sql
INSTALL EXTENSION vsql_metrics_otel_exporter;
SET GLOBAL vsql_metrics_otel_exporter.otlp_endpoint = 'http://127.0.0.1:4318';
SET GLOBAL vsql_metrics_otel_exporter.export_interval_ms = 2000;  -- before enabling
SET GLOBAL vsql_metrics_otel_exporter.enabled = ON;

SHOW GLOBAL STATUS LIKE 'vsql_metrics_otel_exporter.%';
```

`scrapes_total` and `exports_total` should climb together; `scrape_errors_total` and
`export_errors_total` should stay at 0. If an export is failing, `last_export_http_status` gives
the collector's response code and `last_export_curl_errno` a libcurl error number when the
request never completed. If a collector is failing, `last_scrape_error_collector` gives its
1-based position in the registry and the server error log has the message.
