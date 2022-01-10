Name: ofono-ril-binder-plugin
Version: 1.2.6
Release: 1
Summary: Ofono RIL binder transport plugin
License: BSD
URL: https://github.com/mer-hybris/ofono-ril-binder-plugin
Source: %{name}-%{version}.tar.bz2

%define libgrilio_version 1.0.35
%define libgbinder_version 1.0.23
%define libgbinder_radio_version 1.4.1

BuildRequires: pkgconfig
BuildRequires: ofono-devel >= %{ofono_version}
BuildRequires: pkgconfig(libgrilio) >= %{libgrilio_version}

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

Requires: ofono-ril-plugin
Requires: libgrilio >= %{libgrilio_version}

%define plugin_dir %(pkg-config ofono --variable=plugindir)

%description
This package contains ofono plugin which implements binder transport for RIL

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} LIBDIR=%{_libdir} KEEP_SYMBOLS=1 release pkgconfig

%install
rm -rf %{buildroot}
make LIBDIR=%{_libdir} DESTDIR=%{buildroot} install-dev

mkdir -p %{buildroot}/%{plugin_dir}
%preun

%files
%dir %{plugin_dir}
%defattr(-,root,root,-)
%{plugin_dir}/rilbinderplugin.so
%if %{license_support} == 0
%license LICENSE
%endif

#############################################################################

%package -n libgrilio-binder
Summary: Binder based transport for libgrilio
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig
Requires: libgbinder >= %{libgbinder_version}
Requires: libgbinder-radio >= %{libgbinder_radio_version}
BuildRequires: pkgconfig(libgbinder) >= %{libgbinder_version}
BuildRequires: pkgconfig(libgbinder-radio) >= %{libgbinder_radio_version}

%description -n libgrilio-binder
Provides binder based transport for libgrilio

%package -n libgrilio-binder-devel
Summary: Development library for libgrilio-binder
Requires: pkgconfig(glib-2.0)
Requires: pkgconfig(libgrilio) >= %{libgrilio_version}
Requires: pkgconfig(libgbinder) >= %{libgbinder_version}
Requires: pkgconfig(libgbinder-radio) >= %{libgbinder_radio_version}
Requires: libgrilio-binder = %{version}

%post -n libgrilio-binder -p /sbin/ldconfig

%postun -n libgrilio-binder -p /sbin/ldconfig

%description -n libgrilio-binder-devel
This package contains the development library for libgrilio-binder.

%files -n libgrilio-binder
%defattr(-,root,root,-)
%{_libdir}/libgrilio-binder.so.*
%if %{license_support} == 0
%license LICENSE
%endif

%files -n libgrilio-binder-devel
%defattr(-,root,root,-)
%{_libdir}/pkgconfig/libgrilio-binder.pc
%{_libdir}/libgrilio-binder.so
%{_includedir}/grilio-binder/*.h
