#!/bin/bash
# start-keycloak.sh - Start a Keycloak instance with the test realms.
#
# Imports all realm JSON files from test/import/ (pgrealm, wrongrealm).
# Keycloak listens on https://localhost:8443 with a self-signed certificate.
#
# Usage:
#   ./start-keycloak.sh          # start in background
#   ./start-keycloak.sh --stop   # stop the running instance
#
# Admin console: https://localhost:8443/admin (admin/admin)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONTAINER_NAME="kc-test"
KC_PORT=8443
KC_IMAGE="quay.io/keycloak/keycloak:latest"

if command -v podman &>/dev/null; then
  RT=podman
elif command -v docker &>/dev/null; then
  RT=docker
else
  echo "Error: Neither podman nor docker found" >&2
  exit 1
fi

# --- Stop ---

if [ "${1:-}" = "--stop" ]; then
  echo "Stopping Keycloak..."
  $RT stop "$CONTAINER_NAME" 2>/dev/null || true
  $RT rm -f "$CONTAINER_NAME" 2>/dev/null || true
  echo "Done."
  exit 0
fi

# --- Start ---

# Stop any existing instance
$RT stop "$CONTAINER_NAME" 2>/dev/null || true
$RT rm -f "$CONTAINER_NAME" 2>/dev/null || true

echo "Starting Keycloak on https://localhost:$KC_PORT ..."
$RT run -d --rm --name "$CONTAINER_NAME" \
  -p "127.0.0.1:$KC_PORT:8443" \
  -e KC_BOOTSTRAP_ADMIN_USERNAME=admin \
  -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin \
  -e KC_HTTPS_CERTIFICATE_FILE=/keys/crt.pem \
  -e KC_HTTPS_CERTIFICATE_KEY_FILE=/keys/key.pem \
  -v "$SCRIPT_DIR/import:/opt/keycloak/data/import" \
  -v "$SCRIPT_DIR/keys:/keys/" \
  "$KC_IMAGE" \
  start-dev --import-realm >/dev/null

echo "Waiting for Keycloak to start..."
for i in $(seq 1 90); do
  if curl -ksf "https://localhost:$KC_PORT/realms/pgrealm" >/dev/null 2>&1; then
    echo "Keycloak is ready."
    echo ""
    echo "  Realms:"
    echo "    https://localhost:$KC_PORT/realms/pgrealm"
    echo "    https://localhost:$KC_PORT/realms/wrongrealm"
    echo "  Admin:  https://localhost:$KC_PORT/admin (admin/admin)"
    echo ""
    echo "  Stop with: $0 --stop"
    exit 0
  fi
  if [ "$i" -eq 90 ]; then
    echo "Error: Keycloak did not start within 90 seconds" >&2
    $RT logs "$CONTAINER_NAME" 2>&1 | tail -20
    exit 1
  fi
  sleep 1
done
