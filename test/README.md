# OAuth Device Flow Test

End-to-end test for pg_oidc_validator using Keycloak.

## Prerequisites

- PostgreSQL 18 with pg_oidc_validator installed
- Podman or Docker (Podman is preferred and automatically detected; Docker is used as fallback)
- Ruby with bundler
- Chrome/Chromium with chromedriver

## Setup

```bash
bundle install

# Trust the test SSL certificate (required for psql OAuth)
sudo cp keys/crt.pem /usr/local/share/ca-certificates/keycloak-test.crt
sudo update-ca-certificates
```

## Usage

```bash
# Run all tests
PGBIN=/path/to/postgres/bin bundle exec rspec

# Run specific test or with options
PGBIN=/path/to/postgres/bin bundle exec rspec spec/oauth_device_flow_spec.rb
bundle exec rspec --format documentation
```

## What it does

This is currently only a basic successful login test.

1. Sets up fresh PostgreSQL cluster with OAuth configuration
2. Starts Keycloak with pre-configured realm
3. Initiates psql OAuth device flow connection
4. Automatically completes authentication using Selenium
5. Verifies connection works

Auto-cleanup on exit.
