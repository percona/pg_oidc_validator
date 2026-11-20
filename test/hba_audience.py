"""Exercise OAuth audience routing in a disposable, Unix-socket-only PostgreSQL server."""

import base64
import json
import os
import subprocess
import tempfile
import threading
import time
from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa


ISSUER = "https://issuer.invalid"
ROLE = "audience_test_user"
CLIENT = Path(os.getenv("OAUTH_CONN_TEST", Path(__file__).resolve().parents[1] / "oauth_conn_test"))
PGBIN = Path(os.getenv("PGBIN", ""))
# Inherited connection settings such as PGHOSTADDR or PGSERVICE must never
# redirect an integration test to a running database.
ENV = {name: val for name, val in os.environ.items() if not name.startswith("PG")}
ENV.update(LC_ALL="C", no_proxy="127.0.0.1,localhost", NO_PROXY="127.0.0.1,localhost")


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=")


def encode_uint(number):
    return b64url(number.to_bytes((number.bit_length() + 7) // 8, "big")).decode()


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


@contextmanager
def signing_fixture(directory):
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    public = key.public_key().public_numbers()
    directory.mkdir()
    handler = partial(QuietHandler, directory=str(directory))
    with ThreadingHTTPServer(("127.0.0.1", 0), handler) as http:
        base = f"http://127.0.0.1:{http.server_port}"
        discovery = directory / ".well-known"
        discovery.mkdir()
        (discovery / "openid-configuration").write_text(json.dumps({"issuer": ISSUER, "jwks_uri": base + "/keys"}))
        (directory / "keys").write_text(json.dumps({"keys": [{
            "kid": "test", "alg": "RS256", "use": "sig", "kty": "RSA",
            "n": encode_uint(public.n), "e": encode_uint(public.e),
        }]}))
        thread = threading.Thread(target=http.serve_forever, daemon=True)
        thread.start()
        try:
            yield base, key
        finally:
            http.shutdown()
            thread.join()


def access_token(key, audience):
    header = {"alg": "RS256", "kid": "test"}
    claims = {"iss": ISSUER, "aud": audience, "exp": int(time.time()) + 300, "sub": ROLE, "scope": "connect"}
    signed = b".".join(b64url(json.dumps(part).encode()) for part in (header, claims))
    signature = key.sign(signed, padding.PKCS1v15(), hashes.SHA256())
    return (signed + b"." + b64url(signature)).decode()


def run_pg(command, *args):
    return subprocess.run([str(PGBIN / command), *args], env=ENV, check=True, text=True, stdout=subprocess.PIPE).stdout.strip()


def check_login(socket, database, credential, success):
    connection = f"host={socket} dbname={database} user={ROLE} require_auth=oauth oauth_issuer={ISSUER} oauth_client_id=test"
    # This example client only receives short-lived, ephemeral test credentials.
    result = subprocess.run([CLIENT, connection, "SELECT current_database(), current_user", credential],
                            env=ENV, text=True, capture_output=True, timeout=15)
    if success:
        if result.returncode or result.stdout.strip() != f"{database}\t{ROLE}":
            raise RuntimeError(f"Expected successful login to {database}: {result.stderr}")
    elif not result.returncode or f'OAuth bearer authentication failed for user "{ROLE}"' not in result.stderr:
        raise RuntimeError(f"Expected an OAuth rejection from {database}: {result.stderr}")
    print(f"{database}: {'login accepted' if success else 'login rejected'}")


def exercise(directory, discovery, key):
    data = directory / "pgdata"
    run_pg("initdb", "-D", data, "-U", "test_admin", "-A", "trust", "--no-locale")
    major = int((data / "PG_VERSION").read_text().strip())
    global_aud = "resource_a"
    rules = ["local all test_admin trust"]
    for database, aud in (("db_a", global_aud), ("db_b", "resource_b"), ("db_missing", None)):
        rule = f'local {database} {ROLE} oauth issuer="{ISSUER}" scope=connect'
        if major >= 19 and aud is not None:
            rule += f' validator.audience="{aud}"'
        rules.append(rule)
    (data / "pg_hba.conf").write_text("\n".join(rules) + "\n")
    with (data / "postgresql.conf").open("a") as conf:
        conf.write(f"""
listen_addresses = ''
unix_socket_directories = '{directory}'
shared_preload_libraries = 'pg_oidc_validator'
oauth_validator_libraries = 'pg_oidc_validator'
pg_oidc_validator.discovery_url_override = '{discovery}'
pg_oidc_validator.audience = '{global_aud}'
""")
    logfile = directory / "postgres.log"
    try:
        run_pg("pg_ctl", "-D", data, "-l", logfile, "-w", "start")
    except subprocess.CalledProcessError as error:
        raise RuntimeError(logfile.read_text()) from error
    try:
        def admin(sql):
            return run_pg("psql", "-XAt", "-h", directory, "-U", "test_admin", "-d", "postgres",
                          "-v", "ON_ERROR_STOP=1", "-c", sql)
        if admin("SELECT count(*) FROM pg_hba_file_rules WHERE error IS NOT NULL") != "0":
            raise RuntimeError("PostgreSQL rejected the HBA test configuration")
        admin(f"CREATE ROLE {ROLE} LOGIN")
        for database in ("db_a", "db_b", "db_missing"):
            admin("CREATE DATABASE " + database)
        print(f"Testing PostgreSQL {admin('SHOW server_version')}")
        a, b = (access_token(key, aud) for aud in (global_aud, "resource_b"))
        check_login(directory, "db_a", a, True)
        check_login(directory, "db_a", b, False)
        if major >= 19:
            check_login(directory, "db_b", b, True)
            check_login(directory, "db_b", a, False)
            check_login(directory, "db_missing", a, False)
        else:
            check_login(directory, "db_b", a, True)
    finally:
        run_pg("pg_ctl", "-D", data, "-m", "immediate", "-w", "stop")


if __name__ == "__main__":
    # Keep socket paths short and all credentials private to this OS user.
    with tempfile.TemporaryDirectory(prefix="hba-aud-", dir="/tmp") as tmp:
        root = Path(tmp)
        with signing_fixture(root / "issuer") as (url, signing_key):
            exercise(root, url, signing_key)
