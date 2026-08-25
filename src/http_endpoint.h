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

#ifndef VSQL_METRICS_HTTP_ENDPOINT_H
#define VSQL_METRICS_HTTP_ENDPOINT_H

#include <string>

namespace vsql_metrics {
namespace http_endpoint {

// What the caller should do with a request that has been read off the wire.
enum class Route {
  kMetrics,      // GET on the metrics path, authorised
  kNotFound,     // any other path or method
  kUnauthorized  // metrics path, but the bearer token did not match
};

// Opens a listening socket on bind_address:port. Returns -1 on failure and
// fills *error with a reason suitable for the server error log.
//
// Called from the worker's ENABLE wakeup, which the server runs synchronously
// on the SET GLOBAL caller's thread, so a bind failure can be surfaced before
// SET GLOBAL returns rather than discovered later.
int open_listener(const std::string &bind_address, int port, std::string *error);

void close_listener(int fd);

// Accepts one connection. Returns -1 if there was nothing to accept.
int accept_client(int listen_fd);

// Reads a request from an accepted client and classifies it. `expected_token`
// empty means no authentication is required.
//
// Deliberately reads only up to the end of the headers and never a body: this
// runs on the same thread that performs scrapes, so a client that stops sending
// must not be able to hold it. The socket carries a receive timeout.
Route read_request(int client_fd, const std::string &expected_token);

// Writes a complete response and closes the connection.
void respond_metrics(int client_fd, const std::string &body);
void respond_status(int client_fd, int code, const char *reason);

}  // namespace http_endpoint
}  // namespace vsql_metrics

#endif  // VSQL_METRICS_HTTP_ENDPOINT_H
