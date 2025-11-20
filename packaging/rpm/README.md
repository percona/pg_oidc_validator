# RPM Packaging for pg_oidc_validator

This directory contains everything needed to build and test RPM packages for pg_oidc_validator on RHEL-based distributions using **podman**.

## Supported Distributions

- Oracle Linux 8
- Oracle Linux 9

## Prerequisites

- Podman

## Build and Test

Build and test an RPM in one command:

```bash
# Oracle Linux 8 (default)
./packaging/rpm/build-and-test.sh

# Output will be in:
# packaging/rpm/output/el8/pg_oidc_validator-*.rpm

# Oracle Linux 9
./packaging/rpm/build-and-test.sh oraclelinux:9
```
