#pragma once

#include <picojson/picojson.h>

#include <optional>
#include <sstream>
#include <string>

class http_client {
 public:
  http_client();
  ~http_client();

  http_client(const http_client&) = delete;
  http_client& operator=(const http_client&) = delete;

  http_client(http_client&&) = default;
  http_client& operator=(http_client&&) = default;

  picojson::value get_json(const std::string& url);

 private:
  void* curl;
  std::optional<int> last_max_age_;  // Parsed from Cache-Control header

  static std::size_t write_callback(char* contents, std::size_t size, std::size_t nmemb, std::stringstream* userp);
  static std::size_t header_callback(char* buffer, std::size_t size, std::size_t nitems, void* userdata);

  static std::optional<int> parse_cache_control(std::string_view header);
};
