// This is  a simple demo/test executable, NOT part of the main pg_oidc_validator plugin.
// Its only function is to execute a single SQL query using a pre obtained OAuth token.
// With this, it is able to:
// * provide a simple test interface for the validator, without relying on the device flow.
//   (e.g. test/get-token.sh can obtain a token without user interaction)
// * answering the common user questions of "How can we implement a client that do not rely on the device flow".
#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* g_token = nullptr;

static int oauth_hook(PGauthData type, PGconn* conn, void* data) {
  if (type == PQAUTHDATA_OAUTH_BEARER_TOKEN) {
    auto* request = static_cast<PGoauthBearerRequest*>(data);
    request->token = strdup(g_token);
    request->cleanup = [](PGconn*, PGoauthBearerRequest* r) { free(r->token); };
    return 1;
  }
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Usage: %s <connstring> <sql_query> <access_token>\n", argv[0]);
    return 1;
  }

  const char* connstring = argv[1];
  const char* query = argv[2];
  g_token = argv[3];

  PQsetAuthDataHook(oauth_hook);

  PGconn* conn = PQconnectdb(connstring);
  if (PQstatus(conn) != CONNECTION_OK) {
    fprintf(stderr, "Connection error: %s", PQerrorMessage(conn));
    PQfinish(conn);
    return 1;
  }

  PGresult* res = PQexec(conn, query);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    fprintf(stderr, "Query error: %s", PQresultErrorMessage(res));
    PQclear(res);
    PQfinish(conn);
    return 1;
  }

  if (PQntuples(res) > 0) {
    int ncols = PQnfields(res);
    for (int i = 0; i < ncols; i++) {
      if (i > 0) printf("\t");
      printf("%s", PQgetvalue(res, 0, i));
    }
    printf("\n");
  }

  PQclear(res);
  PQfinish(conn);
  return 0;
}
