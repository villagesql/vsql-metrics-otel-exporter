#!/usr/bin/env bash
# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# Installs vsql_metrics_otel_exporter and turns scraping on, once, while the
# server initializes an empty datadir. Both statements record themselves in the
# datadir, so every later start picks them up without this script running again
# -- which is just as well, because the entrypoint only runs
# /docker-entrypoint-initdb.d on an empty datadir.
#
# The entrypoint SOURCES this file rather than executing it (that is what the
# 0644 mode in the Dockerfile buys), so docker_process_sql -- which knows the
# init socket and the root password -- is in scope. For the same reason there is
# no `set -e` and no `exit` here: both would apply to the entrypoint itself.
#
# SET PERSIST rather than a my.cnf line, for two independent reasons:
#   - The variables do not exist until INSTALL EXTENSION registers them, and
#     naming an unknown variable in my.cnf aborts startup (MY-000067).
#   - Even with the loose- prefix, which gets past that, a config-file value
#     lands as the variable's initial value with no OFF->ON transition. The
#     scrape worker starts on that transition, so the server would report
#     scrape_enabled=ON and never tick.

# Single-quoted SQL literals are the only place these values land, so backslash
# and quote are the two characters that can break out.
_vsql_metrics_quote() {
	local v="${1//\\/\\\\}"
	printf '%s' "${v//\'/\\\'}"
}

_vsql_metrics_sql='INSTALL EXTENSION vsql_metrics_otel_exporter;'

# The push path is opt-in. An unset endpoint disables sending outright rather
# than falling back to the extension's own default of http://localhost:4318,
# which in a container is nothing at all -- that default costs a failed export
# every export_interval_ms, forever, and buries a real misconfiguration under
# identical errno-7 noise.
_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.otlp_endpoint\` = '$(_vsql_metrics_quote "${VSQL_METRICS_OTEL_ENDPOINT:-}")';"

# The pull path is on by default, because it is the one that works against a
# plain Prometheus-family scraper with nothing deployed in between. 0.0.0.0
# rather than the extension's 127.0.0.1 default: a scraper is in another pod,
# and a loopback bind is reachable only from inside this container.
#
# Ordered before the enable below. reconcile_listener() reads the address and
# port only on the wakeup where it opens the socket, so changing either while
# the listener is already up does nothing until it is toggled off and back on.
_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.prometheus_bind_address\` = '$(_vsql_metrics_quote "${VSQL_METRICS_PROMETHEUS_BIND_ADDRESS:-0.0.0.0}")';
SET PERSIST \`vsql_metrics_otel_exporter.prometheus_port\` = $((${VSQL_METRICS_PROMETHEUS_PORT:-9104}));
SET PERSIST \`vsql_metrics_otel_exporter.prometheus_enabled\` = ${VSQL_METRICS_PROMETHEUS_ENABLED:-ON};"

if [ -n "${VSQL_METRICS_PROMETHEUS_AUTH_TOKEN:-}" ]; then
	_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.prometheus_auth_token\` = '$(_vsql_metrics_quote "${VSQL_METRICS_PROMETHEUS_AUTH_TOKEN}")';"
fi

# Last, so the first tick already has everything above. This is also what starts
# the worker thread, and the listener opens on its first wakeup.
_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.scrape_enabled\` = ON;"

docker_process_sql <<<"${_vsql_metrics_sql}"

unset _vsql_metrics_sql
unset -f _vsql_metrics_quote
