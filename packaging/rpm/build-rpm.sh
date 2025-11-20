#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BASE_IMAGE=${1:-oraclelinux:8}
DISTRO_NAME="${BASE_IMAGE//:/}"

echo "============================================"
echo "Building RPM for: $BASE_IMAGE"
echo "Output tag: $DISTRO_NAME"
echo "============================================"

podman build --format docker \
    --build-arg BASE_IMAGE="$BASE_IMAGE" \
    -f packaging/rpm/Dockerfile.build \
    -t pg-oidc-builder:$DISTRO_NAME \
    .

CONTAINER_ID=$(podman create pg-oidc-builder:$DISTRO_NAME)
rm -rf packaging/rpm/output/$DISTRO_NAME
mkdir -p packaging/rpm/output/$DISTRO_NAME
podman cp ${CONTAINER_ID}:/output/. packaging/rpm/output/$DISTRO_NAME/
podman rm ${CONTAINER_ID}

echo ""
echo "============================================"
echo "Build complete!"
echo "RPMs available in: packaging/rpm/output/$DISTRO_NAME/"
echo "============================================"
ls -lh packaging/rpm/output/$DISTRO_NAME/
