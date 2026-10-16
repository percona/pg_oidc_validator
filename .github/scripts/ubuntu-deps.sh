#!/bin/bash
#
# Installs what pg_oidc_validator's CI jobs need on top of the prebuilt CI
# images (percona-lab/pg_github_actions_images), which provide PostgreSQL and
# the common dependencies, including what the meson job needs to build
# PostgreSQL in-tree. With "llvm" as the argument it also installs the
# clang-21/libc++ toolchain from apt.llvm.org.

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

# The images carry most of these, so skip apt when there is nothing to install.
if ! dpkg -s "${DEPS[@]}" >/dev/null 2>&1; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${DEPS[@]}"
fi

# apt.llvm.org is a third-party repository, so skip it when the toolchain is
# already present.
if [ "$1" = llvm ] && ! dpkg -s "${LLVM_DEPS[@]}" >/dev/null 2>&1; then
    sudo DEBIAN_FRONTEND=noninteractive bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" -- $LLVM_VERSION
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${LLVM_DEPS[@]}"
fi
