# pg_oidc_validator

OAuth validator library for PostgreSQL 18

This library should support most providers that implement OIDC and provide a valid JWT as an access token.

## Getting started

### Nightly builds

Experimental packages are available on github:

**Debian/Ubuntu:**
- Binaries for Ubuntu 24.04 are available [here](https://github.com/Percona-Lab/pg_oidc_validator/releases/tag/latest).

**RHEL/Oracle Linux/Rocky Linux:**
- RPM packages for OL8 and OL9 are available [here](https://github.com/Percona-Lab/pg_oidc_validator/releases/tag/latest).

### Building from Source

To build and install from sources, use `make USE_PGXS=1 install -j`.

  > **__NOTE__:** A a C++23 compiler and standard library is required to build pg_oidc_validator.

## Configuration

1. Enable the validator in `postgresql.conf`:
  ```
  oauth_validator_libraries=pg_oidc_validator
  pg_oidc_validator.audience = 'postgresql-resource'
  ```
2. Add entries to `pg_hba.conf` that enables OAuth authentication.
  Example:
  ```
  hostssl all             all             127.0.0.1/32            oauth	scope="openid testScope",issuer=https://url.to.the.oidc.issuer
  ```
3. Restart the server
```
pg_ctl -D <datadir> restart
```

## Configuration variables

### pg_oidc_validator.audience

Set this to the audience assigned by your provider to the PostgreSQL resource. The server requires that exact value in the signed access token's `aud` claim (a string, or a member of the audience array). Configure this setting before upgrading. OAuth logins are denied if it is empty. Tokens must also have a valid expiration time.

### pg_oidc_validator.authn_field

This variable controls which field of the provided JWT token is used for identity mapping.
By default this is the `sub` claim, as most providers allow the configuration of this claim to provide different user fields.

In some cases however the `sub` claim is fixed to a randomly generated, application specific identifier which is non known before a user first connects to the application.
This is not practical for mapping provider users to PostgreSQL users in a map file.
Selecting a different `authn_field`, such as `email` allows an easy to use persistent mapping in this case.

### pg_oidc_validator.discovery_url_override

This variable overrides the URL the validator uses for communication with the OIDC provider.
It is mainly intended for testing, but can be also used in environment where the provider has a different external/internal URL.

Setting this variable doesn't change the issuer used during JWT validation.

## Usage

Use a TLS connection to the server with its certificate authority trusted by the client.

Example with `psql`:

```
psql 'host=db.example.com sslmode=verify-full dbname=name oauth_issuer=https://url.to.the.oidc.issuer oauth_client_id=client-id-registered-in-provider oauth_client_secret=optional-client-secret'
```

  > **__NOTE__:** `oauth_client_secret` is only necessary if the provider is configured to require one.

## OIDC provider specific instructions

### Keycloak
Keycloak is easy to run locally using podman or docker for trying things out.

Remember to enable the OAuth 2 device flow for the client configured in Keycloak, since that's the only OAuth flow the command line tools support.

The below example is for a client with id "postgres" and an included scope called "database" being allowed to login with the PostgreSQL access role `rolename`.

Example HBA entry:
```
host	all	rolename	127.0.0.1/32	oauth	scope="database",issuer=https://127.0.0.1:8080/realms/master
```

Connection example:
```
psql 'host=127.0.0.1 dbname=name user=rolename oauth_issuer=https://127.0.0.1:8080/realms/master oauth_client_id=postgres'
```

### Microsoft / Entra ID
* `oauth_issuer` for postgres should be `https://login.microsoftonline.com/<tenant_id>/v2.0`
* For v2 access tokens, set `pg_oidc_validator.audience` to the PostgreSQL resource application's client ID (GUID). Expose a delegated permission on that application, and request its full name, such as `api://<resource-app-id>/connect`, in the OAuth HBA rule. The token must include the corresponding permission in `scp`.
* Use `pg_oidc_validator.authn_field = 'oid'` and map the user's object ID from the configured tenant to a database role in `pg_ident.conf`. Use a `hostssl` rule for those roles and retain the existing HBA rules for service accounts. Leave `delegate_ident_mapping` disabled so PostgreSQL verifies the requested role.

## Google OIDC
Google has some quirks which are currently not supported by the libpq device code implementation.
It is only usable with custom clients that do not rely on the internal device flow.

## Dex
Dex doesn't support OAuth scopes, it doesn't include the parameter in the tokens.
While the use of scopes is strongly recommended for OAuth, and the parameter is required in pg_hba, it can be the empty string.
Specifying `scope=""` disables scope validation.

## Testing

The validator has a basic test suite, documented under [test/README.md](test/README.md).

## Examples

Examples can be found in the `examples` folder.
Currently there's only a single [Keycloak](examples/keycloak/README.md) example.
