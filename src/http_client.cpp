#include "http_client.hpp"

#include "http_cache.hpp"
#include "pg_utils.hpp"

extern "C" {
#include "postgres.h"
#include "utils/elog.h"
}

#include <curl/curl.h>

#include <cassert>
#include <charconv>
#include <regex>
#include <string_view>

http_client::http_client() : curl(curl_easy_init()), last_max_age_(std::nullopt) {
  if (curl == nullptr) {
    throw std::runtime_error("Failed to initialize libcurl");
  }
}

http_client::~http_client() {
  if (curl != nullptr) {
    curl_easy_cleanup(curl);
  }
}

std::size_t http_client::write_callback(char* contents, std::size_t size, std::size_t nmemb, std::stringstream* userp) {
  size_t total_size = size * nmemb;
  *userp << std::string(contents, total_size);
  return total_size;
}

std::size_t http_client::header_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
  // Per curl documentation, size is always 1
  assert(size == 1);

  auto* client = static_cast<http_client*>(userdata);

  std::string_view header(buffer, nitems);
  client->last_max_age_ = parse_cache_control(header);

  return nitems;
}

std::optional<int> http_client::parse_cache_control(std::string_view header) {
  // Parse "Cache-Control: max-age=3600" or similar

  static const std::regex cache_control_regex(R"(^cache-control:.*max-age\s*=\s*(\d+))", std::regex_constants::icase);

  std::string header_str(header);
  std::smatch match{};

  if (std::regex_search(header_str, match, cache_control_regex) && match.size() > 1) {
    const std::string& matched_digits = match[1].str();
    int value = 0;
    const char* begin = matched_digits.data();
    const char* end = &matched_digits[matched_digits.size()];
    auto [ptr, ec] = std::from_chars(begin, end, value);

    if (ec == std::errc()) {
      return value;
    }
  }

  return std::nullopt;
}

picojson::value http_client::get_json(const std::string& url) {
  pg::http_cache& cache = pg::http_cache::get_instance();

  if (auto cached_json = cache.get(url)) {
    picojson::value json_result;
    std::string parse_error = picojson::parse(json_result, *cached_json);

    if (parse_error.empty()) {
      pg::pg_try([&]() { elog(DEBUG1, "HTTP cache hit for URL: %s", url.c_str()); });
      return json_result;
    }

    pg::pg_try([&]() { elog(WARNING, "Cached JSON parsing failed, refetching: %s", parse_error.c_str()); });
    // Fall through to fetch fresh data
  }

  pg::pg_try([&]() { elog(DEBUG1, "HTTP cache miss for URL: %s", url.c_str()); });

  std::stringstream response_data;
  last_max_age_ = std::nullopt;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    throw std::runtime_error("HTTP request failed: " + std::string(curl_easy_strerror(res)));
  }

  long response_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  static constexpr auto http_ok = 200;
  if (response_code != http_ok) {
    throw std::runtime_error("HTTP request returned status code: " + std::to_string(response_code));
  }

  std::string json_string = response_data.str();
  picojson::value json_result;
  std::string parse_error = picojson::parse(json_result, json_string);

  if (!parse_error.empty()) {
    throw std::runtime_error("JSON parsing failed: " + parse_error);
  }

  pg::pg_try([&]() {
    if (last_max_age_.has_value() && last_max_age_.value() > 0) {
      bool cached = cache.put(url, json_string, last_max_age_.value());
      if (cached) {
        elog(DEBUG1, "Cached HTTP response for %d seconds: %s", last_max_age_.value(), url.c_str());
      }
    } else {
      elog(DEBUG2, "Not caching response (no valid Cache-Control): %s", url.c_str());
    }
  });

  return json_result;
}
