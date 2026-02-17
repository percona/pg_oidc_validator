MODULE_big = pg_oidc_validator
PGFILEDESC = "pg_oidc_validator - OAuth token validation for PostgreSQL"

OBJS = \
	src/pg_oidc_validator.o \
	src/http_client.o \
	src/jwk.o

PG_CPPFLAGS = -Ijwt-cpp/include -std=c++23

USE_PGXS ?= 0

ifeq ($(USE_PGXS), 1)
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_oidc_validator
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += -lcurl
ifdef USE_LIBCXX
SHLIB_LINK += -Wl,-Bstatic -lc++ -lc++abi -Wl,-Bdynamic
else
SHLIB_LINK += -static-libstdc++
endif

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(PG_CPPFLAGS) -c -o $@ $<

oauth_conn_test: src/oauth_conn_test.o
	$(CXX) -o $@ $< -L$(libdir) -lpq

src/oauth_conn_test.o: src/oauth_conn_test.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I$(includedir) -std=c++23 -c -o $@ $<
