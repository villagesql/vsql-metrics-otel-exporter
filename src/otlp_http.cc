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

#include "otlp_http.h"

#include <curl/curl.h>

#include <mutex>

namespace vsql_metrics {
namespace otlp_http {
namespace {

std::once_flag g_curl_init_once;

std::size_t discard_body(char *, std::size_t size, std::size_t nmemb, void *) {
  return size * nmemb;  // the collector's response body is of no use here
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// Frees a curl_slist however the enclosing scope exits.
class HeaderList {
 public:
  ~HeaderList() { curl_slist_free_all(list_); }

  HeaderList() = default;
  HeaderList(const HeaderList &) = delete;
  HeaderList &operator=(const HeaderList &) = delete;

  bool append(const char *header) {
    curl_slist *next = curl_slist_append(list_, header);
    if (next == nullptr) return false;
    list_ = next;
    return true;
  }

  curl_slist *get() const { return list_; }

 private:
  curl_slist *list_ = nullptr;
};

// One handle for the life of the process, reused across requests so libcurl's
// connection cache survives: a fresh handle per request means a new TCP (and,
// on an https endpoint, a full TLS) handshake for every chunk of every scrape.
// Only the single scrape worker calls post_json, so no locking is needed.
//
// The handle is deliberately never freed, for the same reason
// curl_global_cleanup is not called: the worker can be disabled and re-enabled,
// and there is no shutdown hook at which freeing it would be safe.
CURL *shared_handle() {
  static CURL *handle = curl_easy_init();
  if (handle != nullptr) curl_easy_reset(handle);
  return handle;
}

}  // namespace

void global_init() {
  // curl_global_cleanup is deliberately never called: the worker can be
  // re-enabled after a disable, and other server code may hold curl state. The
  // one-time initialization is leaked on purpose.
  std::call_once(g_curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string join_url(std::string_view base, std::string_view path) {
  std::string url(base);
  while (!url.empty() && url.back() == '/') url.pop_back();
  url.append(path);
  return url;
}

bool parse_headers(std::string_view spec, std::vector<std::string> *out,
                   std::string *err) {
  out->clear();
  std::size_t pos = 0;
  while (pos <= spec.size()) {
    const std::size_t comma = spec.find(',', pos);
    const std::string_view entry =
        trim(spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                             : comma - pos));
    if (!entry.empty()) {
      if (entry.find('\r') != std::string_view::npos ||
          entry.find('\n') != std::string_view::npos) {
        *err = "otlp_headers must not contain CR or LF";
        out->clear();
        return false;
      }
      const std::size_t eq = entry.find('=');
      if (eq == std::string_view::npos || eq == 0) {
        *err = "otlp_headers entries must be Key=Value, comma separated";
        out->clear();
        return false;
      }
      std::string header(trim(entry.substr(0, eq)));
      header += ": ";
      header.append(trim(entry.substr(eq + 1)));
      out->push_back(std::move(header));
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return true;
}

Response post_json(const std::string &url, const std::string &body,
                   const std::vector<std::string> &headers, long timeout_secs) {
  Response response;

  CURL *curl = shared_handle();
  if (curl == nullptr) {
    response.message = "curl_easy_init failed";
    return response;
  }

  HeaderList header_list;
  if (!header_list.append("Content-Type: application/json")) {
    response.message = "curl_slist_append failed";
    return response;
  }
  for (const std::string &header : headers) {
    if (!header_list.append(header.c_str())) {
      response.message = "curl_slist_append failed";
      return response;
    }
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list.get());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_secs);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_secs);
  // Mandatory off the main thread: without it libcurl's DNS resolution can
  // raise SIGALRM on whichever thread happens to be running.
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  const CURLcode rc = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_status);

  if (rc != CURLE_OK) {
    response.curl_error = static_cast<int>(rc);
    response.message = std::string("OTLP export failed: ") + curl_easy_strerror(rc);
    return response;
  }
  if (response.http_status < 200 || response.http_status >= 300) {
    response.message =
        "collector returned HTTP " + std::to_string(response.http_status);
    return response;
  }

  response.ok = true;
  return response;
}

}  // namespace otlp_http
}  // namespace vsql_metrics
