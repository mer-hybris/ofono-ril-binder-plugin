Name: ofono-ril-binder-plugin
Version: 0.0.9
Release: 1
Summary: Ofono RIL binder transport plugin
Group: Development/Libraries
License: BSD
URL: https://github.com/mer-hybris/ofono-ril-binder-plugin
Source: %{name}-%{version}.tar.bz2

Requires: ofono >= 1.21+git42
Requires: libgrilio >= 1.0.26
BuildRequires: ofono-devel >= 1.21+git42
BuildRequires: pkgconfig(libgrilio) >= 1.0.26

%define plugin_dir %{_libdir}/ofono/plugins

%description
This package contains ofono plugin which implements binder transport for RIL

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release pkgconfig

%install
rm -rf %{buildroot}
make install-dev DESTDIR=%{buildroot}

mkdir -p %{buildroot}/%{plugin_dir}
%preun

%files
%dir %{plugin_dir}
%defattr(-,root,root,-)
%{plugin_dir}/rilbinderplugin.so

#############################################################################

%package -n libgrilio-binder
Summary: Binder based transport for libgrilio
Group: Development/Libraries
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
Requires: libgbinder >= 1.0.23
Requires: libgbinder-radio >= 1.0.4
BuildRequires: pkgconfig(libgbinder-radio) >= 1.0.4

%description -n libgrilio-binder
Provides binder based transport for libgrilio

%package -n libgrilio-binder-devel
Summary: Development library for libgrilio-binder
Requires: pkgconfig(glib-2.0)
Requires: pkgconfig(libgrilio) >= 1.0.26
Requires: pkgconfig(libgbinder) >= 1.0.23
Requires: pkgconfig(libgbinder-radio) >= 1.0.4
Requires: libgrilio-binder = %{version}
Requires: pkgconfig

%post -n libgrilio-binder -p /sbin/ldconfig

%postun -n libgrilio-binder -p /sbin/ldconfig

%description -n libgrilio-binder-devel
This package contains the development library for libgrilio-binder.

%files -n libgrilio-binder
%defattr(-,root,root,-)
%{_libdir}/libgrilio-binder.so.*

%files -n libgrilio-binder-devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/libgrilio-binder.pc
%{_libdir}/libgrilio-binder.so
%{_includedir}/grilio-binder/*.h
