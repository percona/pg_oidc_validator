#!/bin/bash
# generate-realm.sh - Generates test/import realm JSON files programmatically.
#
# This script is the source of truth for the Keycloak test realm configuration.
# It starts a temporary Keycloak instance, configures the realms using kcadm.sh,
# and exports the complete realms (including users with credentials) to JSON.
#
# Realms created:
#   - pgrealm:  The primary test realm.
#   - wrongrealm: An identical realm used to test wrong-issuer scenarios.
#
# To update the realm JSON files, modify this script and re-run it.
#
# Requirements: podman or docker, curl, jq
#
# Usage: ./generate-realm.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMPORT_DIR="$SCRIPT_DIR/import"

CONTAINER_NAME="kc-realm-gen-$$"
EXPORT_CONTAINER="kc-realm-export-$$"
VOLUME_NAME="kc-realm-gen-data-$$"
KC_PORT=18080
KC_IMAGE="quay.io/keycloak/keycloak:latest"

# --- Detect dependencies ---

if command -v podman &>/dev/null; then
  RT=podman
elif command -v docker &>/dev/null; then
  RT=docker
else
  echo "Error: Neither podman nor docker found" >&2
  exit 1
fi

for cmd in curl jq; do
  if ! command -v "$cmd" &>/dev/null; then
    echo "Error: $cmd is required but not found" >&2
    exit 1
  fi
done

echo "Using container runtime: $RT"

# --- Cleanup ---

cleanup() {
  echo "Cleaning up..."
  $RT stop "$CONTAINER_NAME" 2>/dev/null || true
  $RT rm -f "$CONTAINER_NAME" 2>/dev/null || true
  $RT rm -f "$EXPORT_CONTAINER" 2>/dev/null || true
  $RT volume rm "$VOLUME_NAME" 2>/dev/null || true
}
trap cleanup EXIT

# --- Helpers ---

kcadm() {
  $RT exec -i "$CONTAINER_NAME" /opt/keycloak/bin/kcadm.sh "$@"
}

# Configure a realm with the standard test resources:
#   - client scope 'pgscope', 'pgscope2'
#   - client 'pgtest' and 'pgtest2' (public, device flow enabled)
#   - user 'testuser' (testuser@example.com / asdfasdf)
#   - user 'testuser2' (testuser2@example.com / asdfasdf)
#   - role 'pgrole' (assigned to testuser2, required for pgtest2/pgscope2)
setup_realm() {
  local realm=$1

  echo "==> Creating realm '$realm'..."
  kcadm create realms -s "realm=$realm" -s enabled=true

  # kcadm doesn't handle empty-body PUTs well, so we use curl for scope assignments.
  local token
  token=$(curl -sf -X POST "http://localhost:$KC_PORT/realms/master/protocol/openid-connect/token" \
    -d "client_id=admin-cli" \
    -d "username=admin" \
    -d "password=admin" \
    -d "grant_type=password" | jq -r '.access_token')

  echo "    Creating client scopes 'pgscope' and 'pgscope2'..."
  local scope_name scope_id
  for scope_name in pgscope pgscope2; do
    kcadm create client-scopes -r "$realm" -f - <<EOF
{
  "name": "$scope_name",
  "protocol": "openid-connect",
  "attributes": {
    "include.in.token.scope": "true",
    "display.on.consent.screen": "true",
    "consent.screen.text": "PgTest Scope"
  }
}
EOF

    scope_id=$(kcadm get client-scopes -r "$realm" --fields id,name \
      | jq -r --arg n "$scope_name" '.[] | select(.name==$n) | .id')
    echo "    $scope_name ID: $scope_id"

    curl -sf -X PUT \
      "http://localhost:$KC_PORT/admin/realms/$realm/default-optional-client-scopes/$scope_id" \
      -H "Authorization: Bearer $token" \
      -H "Content-Type: application/json"

    # Store IDs for client scope assignment later
    eval "local ${scope_name}_id=$scope_id"
  done
  echo "    Added as default optional client scopes."

  echo "    Creating clients..."
  local client_name client_id
  for client_name in pgtest pgtest2; do
    kcadm create clients -r "$realm" -f - <<EOF
{
  "clientId": "$client_name",
  "name": "$client_name",
  "publicClient": true,
  "standardFlowEnabled": true,
  "directAccessGrantsEnabled": true,
  "serviceAccountsEnabled": false,
  "alwaysDisplayInConsole": true,
  "frontchannelLogout": true,
  "protocol": "openid-connect",
  "redirectUris": ["/*"],
  "webOrigins": ["/*"],
  "attributes": {
    "oauth2.device.authorization.grant.enabled": "true",
    "backchannel.logout.session.required": "true",
    "post.logout.redirect.uris": "+"
  }
}
EOF

    client_id=$(kcadm get clients -r "$realm" --fields id,clientId \
      | jq -r --arg c "$client_name" '.[] | select(.clientId==$c) | .id')
    echo "    $client_name ID: $client_id"

    for scope_name in pgscope pgscope2; do
      eval "scope_id=\$${scope_name}_id"
      curl -sf -X PUT \
        "http://localhost:$KC_PORT/admin/realms/$realm/clients/$client_id/optional-client-scopes/$scope_id" \
        -H "Authorization: Bearer $token" \
        -H "Content-Type: application/json"
    done
  done
  echo "    Assigned pgscope and pgscope2 as optional client scopes."

  echo "    Creating users..."
  local user
  for user in testuser testuser2; do
    kcadm create users -r "$realm" \
      -s "username=$user" \
      -s firstName=Pg \
      -s lastName=User \
      -s "email=$user@example.com" \
      -s emailVerified=true \
      -s enabled=true

    kcadm set-password -r "$realm" \
      --username "$user" \
      --new-password asdfasdf

    echo "    Created $user ($user@example.com)."
  done

  echo "    Creating role 'pgrole'..."
  kcadm create roles -r "$realm" -s name=pgrole

  local role_id
  role_id=$(kcadm get roles -r "$realm" --fields id,name \
    | jq -r '.[] | select(.name=="pgrole") | .id')

  kcadm add-roles -r "$realm" --uusername testuser2 --rolename pgrole
  echo "    Assigned 'pgrole' to testuser2."

  # Add pgrole as scope mapping for pgscope2
  kcadm create "client-scopes/$pgscope2_id/scope-mappings/realm" -r "$realm" -f - <<EOF
[{"id":"$role_id","name":"pgrole"}]
EOF

  # Add pgrole as scope mapping for pgtest2
  local pgtest2_cid
  pgtest2_cid=$(kcadm get clients -r "$realm" --fields id,clientId \
    | jq -r '.[] | select(.clientId=="pgtest2") | .id')
  kcadm create "clients/$pgtest2_cid/scope-mappings/realm" -r "$realm" -f - <<EOF
[{"id":"$role_id","name":"pgrole"}]
EOF
  echo "    Added 'pgrole' as scope mapping for pgtest2 and pgscope2."

  echo "    Realm '$realm' configured."
}

# --- Step 1: Start Keycloak ---

echo "==> Starting Keycloak..."
$RT volume create "$VOLUME_NAME" >/dev/null
$RT run -d --name "$CONTAINER_NAME" \
  -p "127.0.0.1:$KC_PORT:8080" \
  -v "$VOLUME_NAME:/opt/keycloak/data" \
  -e KC_BOOTSTRAP_ADMIN_USERNAME=admin \
  -e KC_BOOTSTRAP_ADMIN_PASSWORD=admin \
  "$KC_IMAGE" start-dev >/dev/null

echo "==> Waiting for Keycloak to start..."
for i in $(seq 1 90); do
  if curl -sf "http://localhost:$KC_PORT/realms/master" >/dev/null 2>&1; then
    echo "    Keycloak is ready."
    break
  fi
  if [ "$i" -eq 90 ]; then
    echo "Error: Keycloak did not start within 90 seconds" >&2
    $RT logs "$CONTAINER_NAME" 2>&1 | tail -20
    exit 1
  fi
  sleep 1
done

# --- Step 2: Authenticate ---

kcadm config credentials \
  --server http://localhost:8080 \
  --realm master \
  --user admin \
  --password admin

# --- Step 3: Create realms ---

setup_realm pgrealm
setup_realm wrongrealm

# --- Step 4: Export realms ---

echo "==> Stopping Keycloak for export..."
$RT stop "$CONTAINER_NAME" >/dev/null

echo "==> Exporting realms..."
# Run kc.sh export in a new container with the same data volume.
# Using --dir so each realm gets its own file, with users included.
$RT run --name "$EXPORT_CONTAINER" \
  -v "$VOLUME_NAME:/opt/keycloak/data" \
  "$KC_IMAGE" \
  export --dir /tmp/export --users realm_file

# Copy the exported realm files from the (stopped) container
$RT cp "$EXPORT_CONTAINER:/tmp/export/pgrealm-realm.json" "$IMPORT_DIR/pgrealm.json"
$RT cp "$EXPORT_CONTAINER:/tmp/export/wrongrealm-realm.json" "$IMPORT_DIR/wrongrealm.json"

echo "==> Realms exported to $IMPORT_DIR/"
echo "    Done!"
