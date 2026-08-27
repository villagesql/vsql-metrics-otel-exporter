# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.

# A VillageSQL server image with vsql_metrics_otel_exporter installed and
# scraping on its own from first start.
#
#   docker build -t villagesql-metrics .
#   docker run --rm -e MYSQL_ROOT_PASSWORD=secret \
#              -e VSQL_METRICS_OTEL_ENDPOINT=http://otel-collector:4318 \
#              -p 3306:3306 villagesql-metrics
#
# VSQL_METRICS_OTEL_ENDPOINT is optional and read once, when the datadir is
# first initialized; without it the extension's own default endpoint
# (http://localhost:4318) applies. Everything else stays at its default,
# including the collector set and the pull endpoint (off).
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

# The pull endpoint, when vsql_metrics_otel_exporter_prometheus_enabled is ON.
# It is off by default and binds 127.0.0.1, so reaching it from outside the
# container also needs prometheus_bind_address set to 0.0.0.0.
EXPOSE 9104
