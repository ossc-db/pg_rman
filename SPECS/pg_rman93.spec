# SPEC file for pg_rman
# Copyright(C) 2009-2015 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-9.3
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share

## Set general information for pg_rman.
Summary:    Backup and Recovery Tool for PostgreSQL
Name:       pg_rman
Version:    1.2.10
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}.tar.gz
URL:        http://sourceforge.net/projects/pg-rman/
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:	    NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql93-devel, zlib-devel
Requires:  postgresql93-libs

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
* Fri Sep  5 2014 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.10-1
* Tue Aug 12 2014 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.9-1
* Sun Apr 20 2014 - The pg_rman Development Group <otsuka.knj@gmail.com> 1.2.8-2
- Fixed the URL.
* Mon Mar 31 2014 - NTT OSS Center <otsuka.kenji@lab.ntt.co.jp> 1.2.8-1
- Update to 1.2.8.
* Fri Dec 28 2013 - NTT OSS Center <otsuka.kenji@lab.ntt.co.jp> 1.2.7-1
- Update to 1.2.7.
- Supporting PostgreSQL 9.3.
- Added required build dependencies. Thanks to Ulrich Habel.
* Mon Sep 2  2013 - NTT OSS Center <otsuka.kenji@lab.ntt.co.jp> 1.2.6-1
- Update to 1.2.6
* Wed Nov 10  2010 - NTT OSS Center <tomonari.katsumata@oss.ntt.co.jp> 1.2.0-1
* Wed Dec 9  2009 - NTT OSS Center <itagaki.takahiro@oss.ntt.co.jp> 1.1.1-1
- Initial cut for 1.1.1
