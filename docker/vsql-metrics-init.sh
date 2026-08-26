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

# The endpoint is left to the deployment: baking a hostname into the image means
# every tick fails against it wherever that name does not resolve. Unset leaves
# the extension's own default (http://localhost:4318).
_vsql_metrics_sql='INSTALL EXTENSION vsql_metrics_otel_exporter;'

if [ -n "${VSQL_METRICS_OTEL_ENDPOINT:-}" ]; then
	# The value reaches a single-quoted SQL literal, so backslash and quote are
	# the two characters that can break out of it.
	_vsql_metrics_ep="${VSQL_METRICS_OTEL_ENDPOINT//\\/\\\\}"
	_vsql_metrics_ep="${_vsql_metrics_ep//\'/\\\'}"
	_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.otlp_endpoint\` = '${_vsql_metrics_ep}';"
fi

# Enabling scraping last means the first tick already has the endpoint above.
_vsql_metrics_sql="${_vsql_metrics_sql}
SET PERSIST \`vsql_metrics_otel_exporter.scrape_enabled\` = ON;"

docker_process_sql <<<"${_vsql_metrics_sql}"

unset _vsql_metrics_sql _vsql_metrics_ep
