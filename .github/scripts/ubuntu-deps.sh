#!/bin/bash
#
# Installs everything the Ubuntu CI jobs need to build pg_oidc_validator, and
# PostgreSQL itself for the in-tree builds.
#
# Jobs running in the shared CI images (percona-lab/pg_github_actions_images)
# call it too: the images provide PostgreSQL and the common build dependencies,
# so the apt-get below is mostly a no-op there, and only the LLVM toolchain is
# actually installed.

set -e

LLVM_VERSION=21

DEPS=(
    # Setup, including what apt.llvm.org's llvm.sh needs
    gnupg
    lsb-release
    software-properties-common
    wget

    # Build
    bison
    build-essential
    flex
    libcurl4-openssl-dev
    libkrb5-dev
    libreadline-dev
    libssl-dev
    libxml2-dev
    libxslt1-dev
    meson
    ninja-build
    pkg-config
    uuid-dev
    zlib1g-dev
)

LLVM_DEPS=(
    clang-$LLVM_VERSION
    clang-tidy-$LLVM_VERSION
    libc++-$LLVM_VERSION-dev
    libc++abi-$LLVM_VERSION-dev
)

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${DEPS[@]}"

# apt.llvm.org is a third-party repository, so skip it when the toolchain is
# already present.
if ! dpkg -s "${LLVM_DEPS[@]}" >/dev/null 2>&1; then
    sudo DEBIAN_FRONTEND=noninteractive bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" -- $LLVM_VERSION
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${LLVM_DEPS[@]}"
fi
