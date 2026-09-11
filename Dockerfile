# Packages pg_oidc_validator as a CloudNativePG image-volume extension image
# (https://cloudnative-pg.io/docs/devel/imagevolume_extensions):
#   lib/       the validator module (CNPG appends <mount>/lib to
#              dynamic_library_path automatically)
#   system/    the libcurl dependency closure — the CloudNativePG postgres
#              image ships no libcurl, so the consuming Cluster must list
#              "system" under the extension's ld_library_path
#   licenses/  this project's license
#
# Built inside the exact postgres server image so the ABI always matches.
# The result only works against the same postgres major and distro it was
# built in; PG_IMAGE and PG_MAJOR must move together.
ARG PG_IMAGE=ghcr.io/cloudnative-pg/postgresql:18.6-system-trixie

FROM $PG_IMAGE AS builder
ARG PG_MAJOR=18
USER root
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential "postgresql-server-dev-${PG_MAJOR}" \
      libcurl4-openssl-dev
COPY . /src
RUN make -C /src USE_PGXS=1 -j"$(nproc)"
# The ldd closure over-copies libraries the postgres image already has —
# harmless, they are the same distro builds and merely shadow themselves.
RUN mkdir -p /out/lib /out/system /out/licenses/pg_oidc_validator \
 && cp /src/pg_oidc_validator.so /out/lib/ \
 && cp /src/LICENSE.txt /out/licenses/pg_oidc_validator/ \
 && for lib in $(ldd /src/pg_oidc_validator.so \
                 | awk '/=> \//{print $3}' \
                 | grep -vE 'libc\.so|ld-linux'); do \
      cp -Ln "$lib" /out/system/; \
    done

FROM scratch
COPY --from=builder /out/ /
