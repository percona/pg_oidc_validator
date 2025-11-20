#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DISTRO=${1:-oraclelinux:8}

"$SCRIPT_DIR/build-rpm.sh" "$DISTRO"
"$SCRIPT_DIR/test-rpm.sh" "$DISTRO"
