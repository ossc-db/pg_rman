# SPEC file for pg_rman
# Copyright(C) 2009-2023 NIPPON TELEGRAPH AND TELEPHONE CORPORATION

%define _pgdir   /usr/pgsql-11
%define _bindir  %{_pgdir}/bin
%define _libdir  %{_pgdir}/lib
%define _datadir %{_pgdir}/share

## Set general information for pg_rman.
Summary:    Backup and Recovery Tool for PostgreSQL
Name:       pg_rman
Version:    1.3.16
Release:    1%{?dist}
License:    BSD
Group:      Applications/Databases
Source0:    %{name}-%{version}-pg11.tar.gz
URL:        https://github.com/ossc-db/pg_rman
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)
Vendor:     NIPPON TELEGRAPH AND TELEPHONE CORPORATION

## We use postgresql-devel package
BuildRequires:  postgresql11-devel, zlib-devel
Requires:  postgresql11-libs

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
%setup -q -n %{name}-%{version}-pg11

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
* Mon Dec  4 2023 - NTT OSS Center <zuowei.yan.tb@hco.ntt.co.jp> 1.3.16-1
* Thu Dec 15 2022 - NTT OSS Center <zuowei.yan.tb@hco.ntt.co.jp> 1.3.15-1
* Mon Dec 13 2021 - NTT OSS Center <masahiro.ikeda.us@hco.ntt.co.jp> 1.3.14-1
* Mon Aug 23 2021 - NTT OSS Center <masahiro.ikeda.us@hco.ntt.co.jp> 1.3.13-1
* Wed Jun  2 2021 - NTT OSS Center <insung.moon.gk@hco.ntt.co.jp> 1.3.12-1
* Tue Jan 19 2021 - NTT OSS Center <insung.moon.gk@hco.ntt.co.jp> 1.3.11-1
* Fri Dec 25 2020 - NTT OSS Center <insung.moon.gk@hco.ntt.co.jp> 1.3.10-1
* Tue Oct 29 2019 - NTT OSS Center <insung.moon.gk@hco.ntt.co.jp> 1.3.9-1
* Tue Jan 22 2019 - NTT OSS Center <moon_insung_i3@lab.ntt.co.jp> 1.3.8-1
* Thu Sep 27 2018 - NTT OSS Center <moon_insung_i3@lab.ntt.co.jp> 1.3.7-1
* Mon Jan 29 2018 - NTT OSS Center <moon_insung_i3@lab.ntt.co.jp> 1.3.6-1
* Fri Jul 14 2017 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.5-1
* Fri Apr 21 2017 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.4-1
* Fri Oct  7 2016 - NTT OSS Center <furutani.kaname@lab.ntt.co.jp> 1.3.3-1
* Mon Jan 25 2016 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.2-1
* Mon Aug 31 2015 - NTT OSS Center <langote_amit_f8@lab.ntt.co.jp> 1.3.1-1
* Thu Jul 30 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.3.0-1
* Wed Jan  7 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.11-1
* Tue Jan  6 2015 - NTT OSS Center <onishi_takashi_d5@lab.ntt.co.jp> 1.2.10-2
- Initial cut for 1.2.10
