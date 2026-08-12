#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# Local test runner: builds the VEB (via build.sh), stages it into a temporary
# veb-dir, and runs the repo's MTR suite against the dev server. Running MTR by
# hand in the dev environment is fiddly for this extension (it needs the VEB
# staged and, for the live test, a reachable OTel Collector), so this wraps it.
#
# The default suite is hermetic (an in-test Python receiver stands in for a
# collector). The skip-gated metrics_live.test runs only when a real collector is
# reachable; this script auto-detects one on :4318 and points the live test at
# its file-exporter output. See test/otel-collector-config.yaml.
#
# Usage:
#   export VillageSQL_BUILD_DIR=$HOME/build/villagesql
#   ./test.sh            # build + run the MTR suite
#   RECORD=1 ./test.sh   # build + record the .result files

set -euo pipefail

VEB="vsql_metrics_otel_exporter.veb"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VILLAGESQL_BUILD_DIR="${VillageSQL_BUILD_DIR:-${VILLAGESQL_BUILD_DIR:-}}"
VEB_DIR="/tmp/${VEB%.veb}-test-veb-$$"

log() { echo "[test] $(date '+%H:%M:%S') - $1"; }
cleanup() { rm -rf "$VEB_DIR" 2>/dev/null || true; }
trap cleanup EXIT

if [[ -z "$VILLAGESQL_BUILD_DIR" ]]; then
    log "ERROR: VillageSQL_BUILD_DIR is not set."
    log "  export VillageSQL_BUILD_DIR=/path/to/villagesql/build"
    exit 1
fi

# Step 1: Build (delegates to build.sh so build logic lives in one place).
log "Building..."
"$SCRIPT_DIR/build.sh"

if [[ ! -f "$SCRIPT_DIR/build/$VEB" ]]; then
    log "ERROR: $VEB not found after build"
    exit 1
fi

# Step 2: Stage the VEB.
mkdir -p "$VEB_DIR"
cp "$SCRIPT_DIR/build/$VEB" "$VEB_DIR/"
log "VEB staged in $VEB_DIR"

# Step 3: The live test skips unless VSQL_METRICS_OTEL_ENDPOINT points at a
# reachable OTel Collector AND VSQL_METRICS_OTEL_OUTFILE names its file-exporter
# output. Auto-detect a local collector (e.g. the vsql-otel docker container,
# whose file exporter writes to ./otel-out/received.json); otherwise the live
# test skips and the suite stays hermetic.
OTEL_EP="${VSQL_METRICS_OTEL_ENDPOINT:-http://127.0.0.1:4318}"
OTEL_OUT="${VSQL_METRICS_OTEL_OUTFILE:-$SCRIPT_DIR/otel-out/received.json}"
# An empty OTLP metrics POST returns 200 when the receiver is up.
if curl -sf -X POST "$OTEL_EP/v1/metrics" \
        -H 'Content-Type: application/json' --data '{}' >/dev/null 2>&1; then
    log "OTel Collector reachable at $OTEL_EP -- live test will run"
    export VSQL_METRICS_OTEL_ENDPOINT="$OTEL_EP"
    export VSQL_METRICS_OTEL_OUTFILE="$OTEL_OUT"
else
    log "No OTel Collector at $OTEL_EP -- live test will skip"
    unset VSQL_METRICS_OTEL_ENDPOINT || true
    unset VSQL_METRICS_OTEL_OUTFILE || true
fi

# Step 4: Run MTR from the dev server's mysql-test dir (MTR manages its own
# server; do NOT start one manually).
MTR_RECORD_ARG=()
if [[ "${RECORD:-0}" == "1" ]]; then
    log "RECORD=1 set -- recording expected results"
    MTR_RECORD_ARG=(--record)
fi

log "Running MTR..."
cd "$VILLAGESQL_BUILD_DIR"
perl ./mysql-test/mysql-test-run.pl \
    --suite="$SCRIPT_DIR/mysql-test" \
    --parallel=1 \
    --nounit-tests \
    --mysqld=--veb-dir="$VEB_DIR" \
    ${MTR_RECORD_ARG[@]+"${MTR_RECORD_ARG[@]}"}

log "Done."
