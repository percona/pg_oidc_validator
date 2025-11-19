#pragma once

extern "C" {
#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/elog.h"
}

#include <array>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>

#include "dshash_wrapper.hpp"
#include "pg_utils.hpp"

namespace pg {

struct http_cache_entry {
  static constexpr size_t url_size = 512;
  static constexpr size_t json_response_size = 8192;

  std::array<char, url_size> url;
  std::array<char, json_response_size> json_response;
  time_t expiration;

  [[nodiscard]] bool is_expired() const { return time(nullptr) >= expiration; }

  static constexpr auto compare = dshash_strcmp;
  static constexpr auto hash = dshash_strhash;
  static constexpr auto copy = dshash_strcpy;

  static const size_t key_size;
  static const size_t entry_size;
};

// TODO: should we use a nested data class in http_cache_entry so these can be inline constexpr?
// or require to split it into key and data classes and have a template helper?
inline const size_t http_cache_entry::key_size = offsetof(http_cache_entry, json_response);
inline const size_t http_cache_entry::entry_size = sizeof(http_cache_entry);

class http_cache {
 private:
  static constexpr size_t max_url_len = http_cache_entry::url_size - 1;
  static constexpr size_t max_json_len = http_cache_entry::json_response_size - 1;
  static constexpr const char* shmem_name = "HttpCache";
  static constexpr const char* tranche_name = "HttpCache";

  std::optional<dshash<http_cache_entry>> cache_;

  http_cache() : cache_(std::nullopt) {}

  static http_cache& instance() {
    static http_cache inst;
    return inst;
  }

 public:
  static http_cache& get_instance() { return instance(); }

  void attach() {
    if (cache_.has_value()) {
      return;
    }

    cache_ = dshash<http_cache_entry>::attach_shared(shmem_name, tranche_name);
  }

  std::optional<std::string> get(const std::string& url) {
    return pg_try([&]() -> std::optional<std::string> {
      if (!cache_.has_value()) {
        return std::nullopt;
      }

      if (url.length() > max_url_len) {
        elog(WARNING, "URL too long for cache: %zu > %zu", url.length(), max_url_len);
        return std::nullopt;
      }

      auto entry_ref = cache_->find(url.c_str(), false);

      if (!entry_ref.has_value()) {
        return std::nullopt;
      }

      if (entry_ref->get()->is_expired()) {
        cache_->erase(url.c_str());
        return std::nullopt;
      }

      return std::string(entry_ref->get()->json_response.data());
    });
  }

  bool put(const std::string& url, const std::string& json_response, int max_age_seconds) {
    return pg_try([&]() -> bool {
      if (!cache_.has_value()) {
        return false;
      }

      if (url.length() > max_url_len) {
        elog(WARNING, "URL too long for cache: %zu > %zu", url.length(), max_url_len);
        return false;
      }

      if (json_response.length() > max_json_len) {
        elog(WARNING, "JSON response too large for cache: %zu > %zu", json_response.length(), max_json_len);
        return false;
      }

      if (max_age_seconds <= 0) {
        return false;
      }

      auto [entry_ref, was_inserted] = cache_->find_or_insert(url.c_str());

      std::strncpy(entry_ref->url.data(), url.c_str(), max_url_len);
      entry_ref->url[max_url_len] = '\0';

      std::strncpy(entry_ref->json_response.data(), json_response.c_str(), max_json_len);
      entry_ref->json_response[max_json_len] = '\0';

      entry_ref->expiration = time(nullptr) + max_age_seconds;

      return true;
    });
  }

  void detach() { cache_.reset(); }
};

}  // namespace pg
