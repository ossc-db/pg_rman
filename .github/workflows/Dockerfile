ARG RHEL_VERSION
FROM rockylinux:${RHEL_VERSION}

ARG RHEL_VERSION
ARG PG_VERSION
ARG PG_RMAN_VERSION

ENV PATH /usr/pgsql-${PG_VERSION}/bin:$PATH
ENV PGDATA /var/lib/pgsql/${PG_VERSION}/data


################################################################################
#
# Prerequisite
#
################################################################################

# Install packages for build
RUN dnf update -y
RUN dnf install -y \
        clang gcc git krb5-devel libselinux-devel libzstd-devel lz4-devel make \
        openssl-devel pam-devel readline-devel rpmdevtools which zlib-devel

# Install PostgreSQL
RUN if [ "${RHEL_VERSION}" = "8" ]; then \
        dnf install -y --enablerepo=powertools perl-IPC-Run; \
    else \
        dnf install -y --enablerepo=crb perl-IPC-Run; \
    fi
RUN dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-${RHEL_VERSION}-x86_64/pgdg-redhat-repo-latest.noarch.rpm
RUN dnf -qy module disable postgresql
RUN dnf install -y postgresql${PG_VERSION}-server postgresql${PG_VERSION}-devel


################################################################################
#
# Build RPMs
#
################################################################################

# Build by postgres user
USER postgres
WORKDIR /var/lib/pgsql/

# Deploy the files required for the build
RUN rpmdev-setuptree
RUN git clone -b REL_${PG_VERSION}_STABLE https://github.com/ossc-db/pg_rman.git
RUN cp -a pg_rman/SPECS/pg_rman${PG_VERSION}.spec rpmbuild/SPECS
RUN cd pg_rman && \
    git archive HEAD \
        --format=tar.gz \
        --prefix=pg_rman-${PG_RMAN_VERSION}-pg${PG_VERSION}/ \
        --output=../rpmbuild/SOURCES/pg_rman-${PG_RMAN_VERSION}-pg${PG_VERSION}.tar.gz

# Build RPMs
RUN rpmbuild rpmbuild/SPECS/pg_rman${PG_VERSION}.spec \
        -bb --define="dist .pg${PG_VERSION}.rhel${RHEL_VERSION}"


################################################################################
#
# Run regression tests
#
################################################################################

USER root
RUN rpm -ivh /var/lib/pgsql/rpmbuild/RPMS/x86_64/*

USER postgres
RUN initdb --no-locale -E UTF8  && \
    pg_ctl -w start && \
    make -C pg_rman installcheck
