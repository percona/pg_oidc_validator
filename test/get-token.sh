#!/bin/bash
# get-token.sh - Get an access token from Keycloak via the password grant.
#
# Usage:
#   ./get-token.sh [options]
#
# Options:
#   -r REALM     Realm name (default: pgrealm)
#   -u USER      Username (default: testuser)
#   -p PASSWORD  Password (default: asdfasdf)
#   -c CLIENT    Client ID (default: pgtest)
#   -s SCOPES    Space-separated scopes (default: "email pgscope")
#   -h HOST      Keycloak base URL (default: https://localhost:8443)
#   -f FIELD     Output a specific field instead of the full JSON response
#                (e.g. "access_token", "refresh_token", "expires_in")
#
# Examples:
#   ./get-token.sh                           # full JSON response
#   ./get-token.sh -f access_token           # just the access token
#   ./get-token.sh -u testuser2 -s "email pgscope pgscope2"
#   ./get-token.sh -r wrongrealm -f access_token

set -euo pipefail

REALM="pgrealm"
USER="testuser"
PASSWORD="asdfasdf"
CLIENT="pgtest"
SCOPES="email pgscope"
HOST="https://localhost:8443"
FIELD=""

while getopts "r:u:p:c:s:h:f:" opt; do
  case $opt in
    r) REALM="$OPTARG" ;;
    u) USER="$OPTARG" ;;
    p) PASSWORD="$OPTARG" ;;
    c) CLIENT="$OPTARG" ;;
    s) SCOPES="$OPTARG" ;;
    h) HOST="$OPTARG" ;;
    f) FIELD="$OPTARG" ;;
    *) echo "Usage: $0 [-r realm] [-u user] [-p password] [-c client] [-s scopes] [-h host] [-f field]" >&2; exit 1 ;;
  esac
done

TOKEN_URL="$HOST/realms/$REALM/protocol/openid-connect/token"

RESPONSE=$(curl -sk -X POST "$TOKEN_URL" \
  -d "grant_type=password" \
  -d "client_id=$CLIENT" \
  -d "username=$USER" \
  -d "password=$PASSWORD" \
  -d "scope=$SCOPES")

if echo "$RESPONSE" | jq -e '.error' >/dev/null 2>&1; then
  echo "Error: $(echo "$RESPONSE" | jq -r '.error_description // .error')" >&2
  exit 1
fi

if [ -n "$FIELD" ]; then
  echo "$RESPONSE" | jq -r --arg f "$FIELD" '.[$f]'
else
  echo "$RESPONSE" | jq .
fi
