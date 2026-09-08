# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(vsql_metrics_otel_exporter C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The preview capabilities this extension needs (sql_query, thread_worker,
# sys_var, status_var) only exist under the SDK's include-dev/ tree. This must
# be set before find_package, which reads it to pick the include root.
set(VillageSQL_USE_DEV_HEADERS ON)
find_package(VillageSQL REQUIRED)

# libcurl for the OTLP/HTTP export (JSON POST to /v1/metrics).
find_package(CURL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "curl: ${CURL_VERSION_STRING}")

add_library(vsql_metrics_otel_exporter SHARED
    src/extension.cc
    src/scrape.cc
    src/otlp_json.cc
    src/otlp_http.cc
    src/value_parse.cc
    src/proc_read.cc
    src/prom_text.cc
    src/http_endpoint.cc
    src/collectors/global_status.cc
    src/collectors/global_variables.cc
    src/collectors/replica_status.cc
    src/collectors/info_schema.cc
    src/collectors/innodb_metrics.cc
    src/collectors/perf_schema_stmt.cc
    src/collectors/perf_schema_waits.cc
    src/collectors/perf_schema_tables.cc
    src/collectors/engine_innodb.cc
    src/collectors/binlog.cc
    src/collectors/process.cc
    src/collectors/villagesql.cc
)
target_compile_options(vsql_metrics_otel_exporter PRIVATE -fPIC)

# VillageSQL_INCLUDE_DIR (not VillageSQLExtensionFramework_INCLUDE_DIR) is what
# FindVillageSQL.cmake repoints at include-dev/ when VillageSQL_USE_DEV_HEADERS
# is on. The framework config's variable always resolves to the stable include/
# tree, which has no preview/sql_query.h.
target_include_directories(vsql_metrics_otel_exporter PRIVATE
    ${VillageSQL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(vsql_metrics_otel_exporter PRIVATE CURL::libcurl)

VEF_CREATE_VEB(
    NAME vsql_metrics_otel_exporter
    LIBRARY_TARGET vsql_metrics_otel_exporter
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
