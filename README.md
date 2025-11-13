# pg\_oidc\_validator 0.1.0

OAuth validator library for PostgreSQL 18

> **__NOTE__:** This library is still experimental and not intended for production use.

This library should support most providers that implement OIDC and provide a valid JWT as an access token.

## Getting started

Binaries for Ubuntu 24.04 are available [here](https://github.com/Percona-Lab/pg_oidc_validator/releases/tag/latest).

To build and install from sources, use `make USE_PGXS=1 install -j`.

  > **__NOTE__:** A a C++23 compiler and standard library is required to build pg_oidc_validator.

## Configuration

1. Enable the validator in `postgresql.conf`:
  ```
  oauth_validator_libraries=pg_oidc_validator
  ```
2. Add entries to `pg_hba.conf` that enables OAuth authentication.
  Example:
  ```
  host    all             all             127.0.0.1/32            oauth	scope="openid testScope",issuer=https://url.to.the.oidc.issuer
  ```
3. Restart the server
```
pg_ctl -D <datadir> restart
```

## GUC variables

### pg_oidc_validator.authn_field

This GUC variable controls which field of the provided JWT token is used for identity mapping.
By default this is the `sub` claim, as most providers allow the configuration of this claim to provide different user fields.

In some cases however the `sub` claim is fixed to a randomly generated, application specific identifier which is non known before a user first connects to the application.
This is not practical for mapping provider users to PostgreSQL users in a map file.
Selecting a different `authn_field`, such as `email` allows an easy to use persistent mapping in this case.

## Usage

Use a connection string with OAuth to connect to the server. Currently only `libpq` clients will support OAuth.

Example with `psql`:

```
psql 'host=127.0.0.1 dbname=name oauth_issuer=https://url.to.the.oidc.issuer oauth_client_id=client-id-registered-in-provider oauth_client_secret=optional-client-secret'
```

  > **__NOTE__:** `oauth_client_secret` is only necessary if the provider is configured to require one.

The `oauth_client_id` is whatever you have registered your postgres instance as in your OIDC provider.

## Setting up your OIDC provider

### Keycloak
Keycloak is easy to run locally using docker for trying things out.

Remember to enable the OAuth 2 device flow for the client you configure in keycloak, since that's the only OAuth flow libpq supports. You don't need to add any URLs in the keycloak configuration as they are not used.

The below example is for a client with id "postgres" and an included scope called "database" being allowed to login with the PostgreSQL access role `rolename`.

Example HBA entry:
```
host	all	rolename	127.0.0.1/32	oauth	scope="database",issuer=http://127.0.0.1:8080/realms/master
```

Connection example:
```
psql 'host=127.0.0.1 dbname=name user=rolename oauth_issuer=https://127.0.0.1:8080/realms/master oauth_client_id=postgres'
```

### Microsoft / Entra ID
* `oauth_issuer` for postgres should be `https://login.microsoftonline.com/<tenant_id>/v2.0`
* It generates different JWTs for providers without custom scopes and with custom scopes.
  The library can only validate JWTs with custom scopes; use the full scope name (api://<application id>/<scope name>) in the `scope` parameter in `pg_hba.conf`

## Google OIDC
Google has some quirks which are currently not supported by the core PostgreSQL oauth code. So for now this extension can unfortunately not support it.

### Other providers
Hopefully the information above will give you everything you need to configure _your_ OIDC provider correctly. If you do, [please let us know](https://forums.percona.com/c/postgresql/) how it went!

## Testing

The validator has a basic test suite, documented under [test/README.md](test/README.md).

## Examples

Examples can be found in the `examples` folder.
Currently there's only a single [keycloak](examples/keycloak/README.md) example.
