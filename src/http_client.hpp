#pragma once

// jwt-cpp defines PICOJSON_USE_INT64 before including picojson.h, adding
// an int64_t member to picojson::_storage.  Without the same define here,
// http_client.o sees a differently-shaped union than the other translation
// units, triggering -Wodr and -Wlto-type-mismatch under LTO (-flto).
// Keep in sync with what jwt-cpp/jwt.h does.
#ifndef PICOJSON_USE_INT64
#define PICOJSON_USE_INT64
#endif
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
