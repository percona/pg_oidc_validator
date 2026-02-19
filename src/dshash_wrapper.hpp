#pragma once

extern "C" {
#include "lib/dshash.h"
#include "postgres.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/dsa.h"
}

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "pg_utils.hpp"

namespace pg {

class dshash_exception : public std::runtime_error {
 public:
  explicit dshash_exception(const std::string& msg) : std::runtime_error(msg) {}
};

template <typename T>
concept dshash_entry = std::is_trivially_copyable_v<T> && requires {
  { T::key_size } -> std::convertible_to<size_t>;
  { T::entry_size } -> std::convertible_to<size_t>;
  { T::compare } -> std::convertible_to<dshash_compare_function>;
  { T::hash } -> std::convertible_to<dshash_hash_function>;
  { T::copy } -> std::convertible_to<dshash_copy_function>;
};

template <dshash_entry Entry>
class dshash {
 private:
  dsa_area* area_;
  dshash_table* table_;
  void* callback_arg_;

  dshash(dsa_area* area, dshash_table* table, void* arg = nullptr) : area_(area), table_(table), callback_arg_(arg) {}

  static dshash_parameters make_parameters(int tranche_id) {
    dshash_parameters params;
    params.key_size = Entry::key_size;
    params.entry_size = Entry::entry_size;
    params.compare_function = Entry::compare;
    params.hash_function = Entry::hash;
    params.copy_function = Entry::copy;
    params.tranche_id = tranche_id;
    return params;
  }

 public:
  struct shared_state {
    LWLock lock;
    int tranche_id{};
    dshash_table_handle table_handle{};
    dsa_handle area_handle{};
  };

  static Size shared_mem_size() { return MAXALIGN(sizeof(shared_state)); }

  class entry_ref {
   private:
    dshash* owner_;
    Entry* entry_;

    friend class dshash;

    entry_ref(dshash* owner, Entry* entry) : owner_(owner), entry_(entry) {}

   public:
    ~entry_ref() {
      if (entry_ && owner_ && owner_->table_) {
        pg_assert_noerror([&]() { dshash_release_lock(owner_->table_, entry_); }, "entry_ref destructor");
      }
    }

    entry_ref(const entry_ref&) = delete;
    entry_ref& operator=(const entry_ref&) = delete;

    entry_ref(entry_ref&& other) noexcept : owner_(other.owner_), entry_(other.entry_) { other.entry_ = nullptr; }

    entry_ref& operator=(entry_ref&& other) noexcept {
      if (this != &other) {
        if (entry_ && owner_ && owner_->table_) {
          pg_assert_noerror([&]() { dshash_release_lock(owner_->table_, entry_); }, "entry_ref move assignment");
        }

        owner_ = other.owner_;
        entry_ = other.entry_;
        other.entry_ = nullptr;
      }
      return *this;
    }

    Entry* operator->() const { return entry_; }
    Entry& operator*() const { return *entry_; }
    Entry* get() const { return entry_; }

    explicit operator bool() const { return entry_ != nullptr; }
  };

  static dshash attach_shared(const char* dsm_name, const char* tranche_name) {
    return pg_try([&]() {
#if PG_VERSION_NUM >= 190000
      auto init_callback = [](void* ptr, void* arg) {
        auto* state = static_cast<shared_state*>(ptr);
        auto* name = static_cast<const char*>(arg);

        int tranche_id = LWLockNewTrancheId(name);

        LWLockInitialize(&state->lock, tranche_id);
        state->tranche_id = tranche_id;
        state->table_handle = DSHASH_HANDLE_INVALID;
        state->area_handle = DSA_HANDLE_INVALID;
      };

      bool found = false;
      void* segment_ptr =
          GetNamedDSMSegment(dsm_name, shared_mem_size(), +init_callback, &found, const_cast<char*>(tranche_name));
#else
      static thread_local const char* init_tranche_name = nullptr;

      auto init_callback = [](void* ptr) {
        auto* state = static_cast<shared_state*>(ptr);

        int tranche_id = LWLockNewTrancheId();
        LWLockRegisterTranche(tranche_id, init_tranche_name);

        LWLockInitialize(&state->lock, tranche_id);
        state->tranche_id = tranche_id;
        state->table_handle = DSHASH_HANDLE_INVALID;
        state->area_handle = DSA_HANDLE_INVALID;
      };

      init_tranche_name = tranche_name;

      bool found = false;
      void* segment_ptr = GetNamedDSMSegment(dsm_name, shared_mem_size(), +init_callback, &found);

      init_tranche_name = nullptr;
#endif

      if (segment_ptr == nullptr) {
        std::string error_msg = "Failed to create or find DSM segment: ";
        error_msg += dsm_name;
        throw dshash_exception(error_msg);
      }

      auto* state = static_cast<shared_state*>(segment_ptr);

      dsa_area* area = nullptr;
      dshash_table* table = nullptr;

      if (state->area_handle == DSA_HANDLE_INVALID) {
        LWLockAcquire(&state->lock, LW_EXCLUSIVE);

        if (state->area_handle == DSA_HANDLE_INVALID) {
          area = dsa_create(state->tranche_id);
          dsa_pin_mapping(area);

          dshash_parameters params = make_parameters(state->tranche_id);
          table = dshash_create(area, &params, nullptr);

          state->table_handle = dshash_get_hash_table_handle(table);
          state->area_handle = dsa_get_handle(area);
        } else {
          area = dsa_attach(state->area_handle);
          dsa_pin_mapping(area);

          dshash_parameters params = make_parameters(state->tranche_id);
          table = dshash_attach(area, &params, state->table_handle, nullptr);
        }

        LWLockRelease(&state->lock);
      } else {
        area = dsa_attach(state->area_handle);
        dsa_pin_mapping(area);

        dshash_parameters params = make_parameters(state->tranche_id);
        table = dshash_attach(area, &params, state->table_handle, nullptr);
      }

      if (table == nullptr) {
        throw dshash_exception("dshash_attach returned null");
      }

      return dshash(area, table, nullptr);
    });
  }

  ~dshash() {
    pg_assert_noerror(
        [&]() {
          if (table_) {
            dshash_detach(table_);
          }
          if (area_) {
            dsa_detach(area_);
          }
        },
        "dshash destructor");
  }

  dshash(const dshash&) = delete;
  dshash& operator=(const dshash&) = delete;

  dshash(dshash&& other) noexcept : area_(other.area_), table_(other.table_), callback_arg_(other.callback_arg_) {
    other.area_ = nullptr;
    other.table_ = nullptr;
  }

  dshash& operator=(dshash&& other) noexcept {
    if (this != &other) {
      pg_assert_noerror(
          [&]() {
            if (table_) {
              dshash_detach(table_);
            }
            if (area_) {
              dsa_detach(area_);
            }
          },
          "dshash move assignment");

      area_ = other.area_;
      table_ = other.table_;
      callback_arg_ = other.callback_arg_;
      other.area_ = nullptr;
      other.table_ = nullptr;
    }
    return *this;
  }

  std::optional<entry_ref> find(const void* key, bool exclusive = false) {
    return pg_try([&]() -> std::optional<entry_ref> {
      auto* entry = static_cast<Entry*>(dshash_find(table_, key, exclusive));

      if (entry == nullptr) {
        return std::nullopt;
      }

      return entry_ref(this, entry);
    });
  }

  std::pair<entry_ref, bool> find_or_insert(const void* key) {
    return pg_try([&]() -> std::pair<entry_ref, bool> {
      bool found = false;
      auto* entry = static_cast<Entry*>(dshash_find_or_insert(table_, key, &found));
      return {entry_ref(this, entry), !found};
    });
  }

  bool erase(const void* key) {
    return pg_try([&]() { return dshash_delete_key(table_, key); });
  }
};

}  // namespace pg
