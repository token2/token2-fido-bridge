Name:           token2-fido-bridge
Version:        0.1.2
Release:        1%{?dist}
Summary:        FIDO2 PC/SC to USB-HID bridge (C++)

License:        MIT
URL:            https://www.token2.com
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  make
BuildRequires:  pcsc-lite-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  libstdc++-static
Requires:       pcsc-lite

%description
token2-fido-bridge lets browsers use PC/SC smartcards (contact or NFC) as
FIDO2/WebAuthn security keys on Linux. It creates a virtual USB-HID FIDO2
authenticator via the Linux uhid interface and relays CTAP2/U2F traffic to
the card over PC/SC. Works with Chromium/Chrome and, via a bundled udev rule,
Snap-confined Firefox.

%prep
%autosetup

%build
%cmake -DSTATIC_CXX_RUNTIME=ON
%cmake_build

%install
%cmake_install

%post
/sbin/modprobe uhid 2>/dev/null || :
udevadm control --reload-rules 2>/dev/null || :
udevadm trigger 2>/dev/null || :
%systemd_post token2-fido-bridge.service
if [ $1 -eq 1 ]; then
    systemctl enable --now token2-fido-bridge.service 2>/dev/null || :
fi

%preun
%systemd_preun token2-fido-bridge.service

%postun
%systemd_postun_with_restart token2-fido-bridge.service

%files
%license LICENSE
%doc README.md
%{_bindir}/token2-fido-bridge
%{_prefix}/lib/systemd/system/token2-fido-bridge.service
%{_prefix}/lib/udev/rules.d/70-token2-fido-bridge.rules
%{_prefix}/lib/modules-load.d/token2-fido-bridge-uhid.conf

%changelog
* Tue Jul 14 2026 Token2 <support@token2.com> - 0.1.2-1
- Publish .rpm packages alongside .deb in releases.
* Tue Jul 14 2026 Token2 <support@token2.com> - 0.1.1-1
- Fix snap-confined Firefox support via KERNELS-based udev tagging.
- CI: build and publish .rpm packages.
- Declare libstdc++-static build dependency for static linking.

* Mon Jul 13 2026 Token2 <support@token2.com> - 0.1.0-1
- Initial C++ port with configurable VID/PID (default Token2 349e:0001).
