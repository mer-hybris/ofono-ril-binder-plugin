Name: ofono-ril-binder-plugin
Version: 0.0.1
Release: 1
Summary: Ofono RIL binder transport plugin
Group: Development/Libraries
License: GPLv2
URL: https://git.merproject.org/mer-core/ofono-ril-binder-plugin
Source: %{name}-%{version}.tar.bz2

Requires: ofono >= 1.21+git28
Requires: libgrilio >= 1.0.25
BuildRequires: pkgconfig(libgbinder)
BuildRequires: pkgconfig(libgrilio) >= 1.0.25
BuildRequires: ofono-devel >= 1.21+git28

%define plugin_dir %{_libdir}/ofono/plugins

%description
This package contains ofono plugin which implements binder transport for RIL

%prep
%setup -q -n %{name}-%{version}

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
%make_install

mkdir -p %{buildroot}/%{plugin_dir}
%preun

%files
%dir %{plugin_dir}
%defattr(-,root,root,-)
%{plugin_dir}/rilbinderplugin.so
