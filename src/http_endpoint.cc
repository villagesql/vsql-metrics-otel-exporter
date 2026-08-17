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

// The listening socket behind GET /metrics.
//
// There is no thread here. The socket is handed to the SDK's thread worker as
// its poll fd, so the one worker thread that performs scrapes is also the one
// woken when a scraper connects. That is why every operation in this file is
// bounded: anything that blocks here delays the metrics export running on the
// same thread.

#include "http_endpoint.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string_view>

namespace vsql_metrics {
namespace http_endpoint {
namespace {

// A scraper's request is a few hundred bytes. Anything beyond this is not a
// Prometheus scrape and is refused rather than buffered.
constexpr std::size_t kMaxRequestBytes = 4096;

// Bounds how long a client can hold the scrape thread. Deliberately short: the
// worker cannot export metrics while it is here.
constexpr int kClientTimeoutSecs = 2;

void set_timeout(int fd, int option, int secs) {
  timeval tv{};
  tv.tv_sec = secs;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
}

void write_all(int fd, const char *data, std::size_t len) {
  std::size_t written = 0;
  while (written < len) {
    const ssize_t n = send(fd, data + written, len - written, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (n == 0) return;
    written += static_cast<std::size_t>(n);
  }
}

// Case-insensitive search for a header, returning its value trimmed of leading
// spaces. Header names are case-insensitive per RFC 9110.
std::string_view find_header(std::string_view request, std::string_view name) {
  std::size_t pos = 0;
  while (pos < request.size()) {
    const std::size_t eol = request.find("\r\n", pos);
    const std::string_view line = request.substr(
        pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
    if (line.size() > name.size() &&
        strncasecmp(line.data(), name.data(), name.size()) == 0 &&
        line[name.size()] == ':') {
      std::string_view value = line.substr(name.size() + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
      }
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
      }
      return value;
    }
    if (eol == std::string_view::npos) break;
    pos = eol + 2;
  }
  return {};
}

// Constant-time comparison, so a token cannot be recovered by timing the
// response. Both operands are short, so the cost is irrelevant.
bool tokens_match(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace

int open_listener(const std::string &bind_address, int port, std::string *error) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("socket() failed: ") + std::strerror(errno);
    return -1;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    *error = "prometheus_bind_address is not a valid IPv4 address: " + bind_address;
    close(fd);
    return -1;
  }

  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    *error = "could not bind " + bind_address + ":" + std::to_string(port) +
             ": " + std::strerror(errno);
    close(fd);
    return -1;
  }
  if (listen(fd, 16) < 0) {
    *error = std::string("listen() failed: ") + std::strerror(errno);
    close(fd);
    return -1;
  }
  return fd;
}

void close_listener(int fd) {
  if (fd >= 0) close(fd);
}

int accept_client(int listen_fd) {
  if (listen_fd < 0) return -1;
  const int client = accept(listen_fd, nullptr, nullptr);
  if (client < 0) return -1;
  set_timeout(client, SO_RCVTIMEO, kClientTimeoutSecs);
  set_timeout(client, SO_SNDTIMEO, kClientTimeoutSecs);
  return client;
}

Route read_request(int client_fd, const std::string &expected_token) {
  char buffer[kMaxRequestBytes];
  std::size_t total = 0;

  // Stops at the end of the headers. A body is never read: nothing this
  // endpoint serves depends on one, and reading it would let a client with a
  // large Content-Length occupy the scrape thread.
  while (total < sizeof(buffer) - 1) {
    const ssize_t n = recv(client_fd, buffer + total, sizeof(buffer) - 1 - total, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Route::kNotFound;
    }
    if (n == 0) break;
    total += static_cast<std::size_t>(n);
    buffer[total] = '\0';
    if (std::strstr(buffer, "\r\n\r\n") != nullptr) break;
  }
  buffer[total] = '\0';
  const std::string_view request(buffer, total);

  // Only GET is served. The request line is "GET <path> HTTP/1.1".
  if (request.rfind("GET ", 0) != 0) return Route::kNotFound;
  const std::size_t path_start = 4;
  const std::size_t path_end = request.find(' ', path_start);
  if (path_end == std::string_view::npos) return Route::kNotFound;

  std::string_view path = request.substr(path_start, path_end - path_start);
  // A query string is ignored rather than rejected; Prometheus does not send
  // one, but proxies sometimes append a cache-buster.
  const std::size_t query = path.find('?');
  if (query != std::string_view::npos) path = path.substr(0, query);
  if (path != "/metrics") return Route::kNotFound;

  if (expected_token.empty()) return Route::kMetrics;

  const std::string_view authorization = find_header(request, "Authorization");
  constexpr std::string_view kBearer = "Bearer ";
  if (authorization.size() <= kBearer.size() ||
      strncasecmp(authorization.data(), kBearer.data(), kBearer.size()) != 0) {
    return Route::kUnauthorized;
  }
  return tokens_match(authorization.substr(kBearer.size()), expected_token)
             ? Route::kMetrics
             : Route::kUnauthorized;
}

void respond_metrics(int client_fd, const std::string &body) {
  std::string head =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
      "Content-Length: " +
      std::to_string(body.size()) +
      "\r\n"
      "Connection: close\r\n"
      "\r\n";
  write_all(client_fd, head.data(), head.size());
  write_all(client_fd, body.data(), body.size());
}

void respond_status(int client_fd, int code, const char *reason) {
  std::string response = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                         "\r\nContent-Length: 0\r\n";
  // Tells a scraper how to authenticate rather than leaving it guessing.
  if (code == 401) response += "WWW-Authenticate: Bearer\r\n";
  response += "Connection: close\r\n\r\n";
  write_all(client_fd, response.data(), response.size());
}

}  // namespace http_endpoint
}  // namespace vsql_metrics
