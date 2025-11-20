Name:           pg_oidc_validator
Version:        0.1
Release:        1%{?dist}
Summary:        PostgreSQL OAuth/OIDC token validator extension

%global debug_package %{nil}

License:        Apache-2.0
URL:            https://github.com/Percona-Lab/pg_oidc_validator
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-toolset-13
BuildRequires:  postgresql18-devel
BuildRequires:  libcurl-devel
BuildRequires:  openssl-devel

Requires:       postgresql18
Requires:       libcurl
Requires:       openssl-libs

%description
pg_oidc_validator is a PostgreSQL extension that implements OIDC (OpenID Connect)
token validation. It validates JWT tokens from OIDC providers, enabling OAuth-based
authentication for PostgreSQL connections.

%prep
%setup -q

%build
source /opt/rh/gcc-toolset-13/enable
export PG_CONFIG=/usr/pgsql-18/bin/pg_config
make USE_PGXS=1 %{?_smp_mflags} with_llvm=no COMPILER='g++ $(CXXFLAGS)'

%install
source /opt/rh/gcc-toolset-13/enable
export PG_CONFIG=/usr/pgsql-18/bin/pg_config
make USE_PGXS=1 install DESTDIR=%{buildroot} with_llvm=no COMPILER='g++ $(CXXFLAGS)'

%files
%license LICENSE.txt
%doc README.md
/usr/pgsql-18/lib/pg_oidc_validator.so

%changelog
* Thu Nov 14 2025 Percona <info@percona.com> - 0.1-1
- Initial RPM package
