#!/bin/bash

set -e

LLVM_VERSION=21

LLVM_DEPS=(
    clang-$LLVM_VERSION
    clang-tidy-$LLVM_VERSION
    libc++-$LLVM_VERSION-dev
    libc++abi-$LLVM_VERSION-dev
)

if ! dpkg -s libcurl4-openssl-dev >/dev/null 2>&1; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y libcurl4-openssl-dev
fi

if [ "$1" = llvm ]; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y software-properties-common
    sudo DEBIAN_FRONTEND=noninteractive bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)" -- $LLVM_VERSION
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${LLVM_DEPS[@]}"
fi
