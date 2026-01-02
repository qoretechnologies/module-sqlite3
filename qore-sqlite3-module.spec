%define module_api %(qore --module-api 2>/dev/null)
%define module_dir %{_libdir}/qore-modules

%if 0%{?sles_version}

%define dist .sles%{?sles_version}

%else
%if 0%{?suse_version}

# get *suse release major version
%define os_maj %(echo %suse_version|rev|cut -b3-|rev)
# get *suse release minor version without trailing zeros
%define os_min %(echo %suse_version|rev|cut -b-2|rev|sed s/0*$//)

%if %suse_version > 1010
%define dist .opensuse%{os_maj}_%{os_min}
%else
%define dist .suse%{os_maj}_%{os_min}
%endif

%endif
%endif

# see if we can determine the distribution type
%if 0%{!?dist:1}
%define rh_dist %(if [ -f /etc/redhat-release ];then cat /etc/redhat-release|sed "s/[^0-9.]*//"|cut -f1 -d.;fi)
%if 0%{?rh_dist}
%define dist .rhel%{rh_dist}
%else
%define dist .unknown
%endif
%endif

# see if we can determine the distribution type
%if 0%{!?dist:1}
%define rh_dist %(if [ -f /etc/redhat-release ];then cat /etc/redhat-release|sed "s/[^0-9.]*//"|cut -f1 -d.;fi)
%if 0%{?rh_dist}
%define dist .rhel%{rh_dist}
%else
%define dist .unknown
%endif
%endif

Summary: SQLite3 DBI module for Qore
Name: qore-sqlite3-module
Version: 1.2.0
Release: 1%{dist}
License: LGPL
Group: Development/Languages
URL: http://www.qore.org
Source: http://prdownloads.sourceforge.net/qore/%{name}-%{version}.tar.bz2
#Source0: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
Requires: /usr/bin/env
Requires: qore-module-api-%{module_api}
BuildRequires: gcc-c++
%if 0%{?el7}
BuildRequires:  devtoolset-7-gcc-c++
%endif
BuildRequires: qore-devel
BuildRequires: qore
BuildRequires: openssl-devel
# Sqlite RPM package name are different in distros
%if 0%{?suse_version}
Requires: sqlite3
BuildRequires: sqlite3-devel
%else
Requires: sqlite > 3.0
BuildRequires: sqlite-devel > 3.0
%endif

%description
Sqlite3 DBI driver module for the Qore Programming Language.


%if 0%{?suse_version}
%debug_package
%endif

%prep
%setup -q

%build
%if 0%{?el7}
# enable devtoolset7
. /opt/rh/devtoolset-7/enable
unset PATH
%endif
export CXXFLAGS="%{?optflags}"
cmake -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_BUILD_TYPE=RELWITHDEBINFO -DCMAKE_SKIP_RPATH=1 -DCMAKE_SKIP_INSTALL_RPATH=1 -DCMAKE_SKIP_BUILD_RPATH=1 -DCMAKE_PREFIX_PATH=${_prefix}/lib64/cmake/Qore .
make %{?_smp_mflags}
make docs

%install
make DESTDIR=%{buildroot} install %{?_smp_mflags}

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%{module_dir}
%doc COPYING COPYING.MIT README AUTHORS

%package doc
Summary: SQLite3 DBI module for Qore
Group: Development/Languages

%description doc
SQLite3 module for the Qore Programming Language.

This RPM provides API documentation, test and example programs

%files doc
%defattr(-,root,root,-)
%doc docs/sqlite3/html test/basic.qtest test/sqlite3test-threading.q test/blob.png

%changelog
* Thu Jan 2 2026 David Nichols <david.nichols@qoretechnologies.com>
- updated to version 1.2.0
- fixed BLOB binding memory safety issue (use SQLITE_TRANSIENT)
- fixed integer truncation (use sqlite3_column_int64)
- fixed quote handling in SQL parser
- added proper error checking for sqlite3_step()
- added DBI_CAP_HAS_STATEMENT capability
- added GitLab CI with k8s runner
- improved documentation

* Mon May 2 2022 David Nichols <david.nichols@qoretechnologies.com>
- updated to version 1.1.0

* Tue Jul 13 2021 David Nichols <david.nichols@qoretechnologies.com>
- updated to version 1.0.2

* Fri Apr 16 2010 Petr Vanek <petr.vanek@qoretechnologies.com>
- updated to version 1.0.1

* Fri Jun 5 2009 Petr Vanek <petr.vanek@qoretechnologies.com>
- initial spec file for separate sqlite3 release
