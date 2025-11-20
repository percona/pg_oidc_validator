#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BASE_IMAGE=${1:-oraclelinux:8}
LOG_OUTPUT_DIR="$REPO_ROOT/test-logs/"
DISTRO_NAME="${BASE_IMAGE/:/}"

RPM_DIR="packaging/rpm/output/$DISTRO_NAME"
if [ ! -d "$RPM_DIR" ]; then
    echo "Error: RPM directory not found: $RPM_DIR"
    echo "Run ./packaging/rpm/build-rpm.sh first"
    exit 1
fi

RPM_FILE=$(find "$RPM_DIR" -name '*.rpm' ! -name '*.src.rpm' | head -1)
if [ -z "$RPM_FILE" ]; then
    echo "Error: No RPM file found in $RPM_DIR"
    exit 1
fi

echo "============================================"
echo "Testing RPM: $RPM_FILE"
echo "Base image: $BASE_IMAGE"
echo "============================================"

cp "$RPM_FILE" packaging/rpm/pg_oidc_validator-test.rpm

#podman build --format docker \
docker build \
    --build-arg BASE_IMAGE="$BASE_IMAGE" \
    --build-arg RPM_FILE=packaging/rpm/pg_oidc_validator-test.rpm \
    -f packaging/rpm/Dockerfile.test \
    -t pg-oidc-tester:$DISTRO_NAME \
    .

echo ""
echo "============================================"
echo "Running tests..."
echo "Test suite will automatically start Keycloak using podman"
echo "============================================"
echo ""

mkdir -p "$LOG_OUTPUT_DIR"
docker run $RUN_ARGS --rm --privileged \
    -v "$LOG_OUTPUT_DIR:/test-logs:Z" \
    pg-oidc-tester:$DISTRO_NAME

echo ""
echo "============================================"
echo "Tests completed successfully!"
echo "============================================"
