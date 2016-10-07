# SPEC file for pg_rman
# Copyright(C) 2009-2015 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-9.5
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share

## Set general information for pg_rman.
Summary:    Backup and Recovery Tool for PostgreSQL
Name:       pg_rman
Version:    1.3.3
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
URL:        https://github.com/ossc-db/pg_rman
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:	    NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql95-devel, zlib-devel
Requires:  postgresql95-libs

## Description for "pg_rman"
%description
pg_rman manages backup and recovery of PostgreSQL.
pg_rman has the features below:
-Takes a backup while database including tablespaces with just one command. 
-Can recovery from backup with just one command. 
-Supports incremental backup and compression of backup files so that it takes less disk spaces. 
-Manages backup generations and shows a catalog of the backups. 


## pre work for build pg_rman
%prep
%setup -q -n %{name}-%{version}

## Set variables for build environment
%build
PATH=%{_bindir}:$PATH USE_PGXS=1 make %{?_smp_mflags}

## Set variables for install
%install
rm -rf %{buildroot}

PATH=%{_bindir}:$PATH USE_PGXS=1 DESTDIR=%{buildroot} make %{?_smp_mflags} install

install -d %{buildroot}%{_bindir}
install -m 755 pg_rman %{buildroot}%{_bindir}/pg_rman

%clean
rm -rf %{buildroot}

%files
%defattr(755,root,root)
%{_bindir}/pg_rman

# History of pg_rman.
%changelog
* Fri Oct  7 2016 - NTT OSS Center <furutani.kaname@lab.ntt.co.jp> 1.3.3-1
* Mon Jan 25 2016 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.2-1
* Mon Aug 31 2015 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.1-1
* Thu Jul 30 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.3.0-1
* Wed Jan  7 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.11-1
* Tue Jan  6 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.10-2
- Initial cut for 1.2.10
