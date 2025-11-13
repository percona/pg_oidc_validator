# Keycloak Example for pg_oidc_validator

This example provides a demo environment with Keycloak and PostgreSQL configured for OAuth authentication using Docker Compose.

**Notes:**
* This is only a demo environment, with self-signed certificates.
  Do not use for production.
* This example doesn't use persistent volumes. When you stop the containers, all data is lost.

## Prerequisites

### Required software

Docker with Compose V2 installed (`docker compose` command)

### Add keycloak to hosts file

For the OAuth device flow to work in your browser, add `keycloak` to your hosts file:

**On Linux/Mac:**
```bash
echo "127.0.0.1 keycloak" | sudo tee -a /etc/hosts
```

**On Windows:** Edit `C:\Windows\System32\drivers\etc\hosts` as Administrator and add:
```
127.0.0.1 keycloak
```

This allows your browser to resolve the `https://keycloak:8443` URL used in the OAuth device flow.

## Quick Start

```bash
# Start Keycloak and PostgreSQL
docker compose up -d

# Wait for services to be ready
# Optional: watch the logs while waiting
docker compose logs -f

# Once ready, run the interactive psql client
# Follow the OAuth device flow, login with `testuser` / `asdfasdf`
docker compose run --rm psql-client

# When done, stop the services
docker compose down
```

## Keycloak admin interface access

Open https://keycloak:8443 in your browser:
- Username: `admin`
- Password: `admin`
