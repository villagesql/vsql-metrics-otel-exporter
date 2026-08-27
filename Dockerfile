# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

# A VillageSQL server image with vsql_metrics_otel_exporter installed, scraping,
# and serving Prometheus text on :9104 from first start.
#
#   docker build -t villagesql-metrics .
#   docker run --rm -e MYSQL_ROOT_PASSWORD=secret \
#              -p 3306:3306 -p 9104:9104 villagesql-metrics
#
# The pull endpoint is the default because it is the half that works against a
# Prometheus-family scraper with nothing deployed in between. The OTLP push is
# opt-in via VSQL_METRICS_OTEL_ENDPOINT, and needs a receiver that accepts
# protobuf-JSON: an OpenTelemetry Collector does, and VictoriaMetrics' own
# /opentelemetry/v1/metrics does not ("json encoding isn't supported for
# opentelemetry format"), so pointing this at a vmsingle will not work.
#
# All of it is read once, when the datadir is first initialized. The knobs, each
# defaulted in docker/vsql-metrics-init.sh:
#
#   VSQL_METRICS_OTEL_ENDPOINT             unset -> push disabled
#   VSQL_METRICS_PROMETHEUS_ENABLED        ON
#   VSQL_METRICS_PROMETHEUS_BIND_ADDRESS   0.0.0.0
#   VSQL_METRICS_PROMETHEUS_PORT           9104
#   VSQL_METRICS_PROMETHEUS_AUTH_TOKEN     unset -> no Authorization required
#
# The install and the enable live in the datadir, not in this image: a container
# started against a volume that some earlier image already initialized comes up
# with the VEB staged but nothing installed. See docker/vsql-metrics-init.sh.

ARG VSQL_IMAGE=villagesql/server:stable

# ---------------------------------------------------------------------------
# Builder: the server image already ships the extension SDK (headers under
# /usr/include-dev, VEF_CREATE_VEB via /usr/lib/cmake, villagesql_config in
# PATH), so find_package(VillageSQL) resolves with no VillageSQL_BUILD_DIR.
# Building here rather than copying a host-built .veb keeps the binary matched
# to the server's ABI and to Ubuntu 24.04's libcurl/libstdc++.
# ---------------------------------------------------------------------------
FROM ${VSQL_IMAGE} AS builder

USER root

# cmake, g++ and make are already in the base image; libcurl's headers are not.
# All four are named so the build does not silently depend on what the base
# happens to ship.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        cmake \
        g++ \
        make \
        libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/src

# Only the build inputs, so an edit to README/mysql-test does not bust the layer.
COPY CMakeLists.txt manifest.json ./
COPY cmake/ ./cmake/
COPY src/ ./src/

RUN cmake -S /build/src -B /build/out \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DVillageSQL_VEB_INSTALL_DIR=/build/veb \
    && cmake --build /build/out -- -j"$(nproc)" \
    && test -f /build/out/vsql_metrics_otel_exporter.veb

# ---------------------------------------------------------------------------
# Runtime: base image plus the bundle. /usr/lib/veb is the server's default
# --veb-dir and is owned by mysql (uid 999), which is who mysqld reads it as.
# ---------------------------------------------------------------------------
FROM ${VSQL_IMAGE}

COPY --from=builder --chown=mysql:mysql --chmod=0644 \
     /build/out/vsql_metrics_otel_exporter.veb /usr/lib/veb/

# This extension uses preview capabilities (sql_query, thread_worker, sys_var,
# status_var), which the server refuses to load unless the flag is on. It cannot
# be turned on with SET GLOBAL -- only SET PERSIST or a startup option -- so the
# image sets it here; without it INSTALL EXTENSION fails with ER 3219.
#
# /etc/mysql/my.cnf, not /etc/my.cnf, although the base image ships neither.
# mysqld reads /etc/my.cnf, /etc/mysql/my.cnf, /usr/etc/my.cnf, ~/.my.cnf in that
# order, and orchestrators that hand the server a config -- the vcloud operator
# among them -- mount theirs at /etc/my.cnf, which shadows anything the image put
# there. The second path is left alone by those, and being read later it also
# wins over a value set in the first.
RUN mkdir -p /etc/mysql \
    && printf '%s\n' \
        '[mysqld]' \
        'vsql-allow-preview-extensions=ON' \
        > /etc/mysql/my.cnf \
    && chmod 0644 /etc/mysql/my.cnf

# INSTALL EXTENSION and the enable, run once against an empty datadir. 0644 (not
# 0755) is deliberate: the entrypoint sources non-executable init scripts, which
# is what puts docker_process_sql in the script's scope.
COPY --chmod=0644 docker/vsql-metrics-init.sh \
     /docker-entrypoint-initdb.d/10-vsql-metrics.sh

# The pull endpoint. Documentation only -- nothing consults it -- but under
# Kubernetes the matching containerPort is worth declaring and worth naming
# `metrics`, which is what the conventional annotation-free pod scrape job
# selects on.
EXPOSE 9104
