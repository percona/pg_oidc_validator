# Changelog

## [1.1.0]

### Added

- PostgreSQL 19 support
- Support for discovery URL override (Mark Bøgelund)
- Allow users to specify `scope` as the empty string to opt out of scope validation

### Fixed

- Fixed build failures on GCC 11 (Devrim Gunduz)

## 1.0.0

### Added

- Library name and version is now in the `pg_get_loaded_modules()` output
- Possibility to specify `USE_LIBCXX` to dynamically link to `libstdc++`

## Fixed

- Crash due to DSA not being pinned
- Detach HTTP cache before shmem_exit

## 0.2

### Added

- Caching of HTTP requests
- A sample Docker Compose configuration

## 0.1

Initial version
