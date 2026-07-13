Name:           token2-fido-bridge
Version:        0.1.0
Release:        1%{?dist}
Summary:        FIDO2 PC/SC to USB-HID bridge (C++)

License:        MIT
URL:            https://www.token2.com
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  pcsc-lite-devel
BuildRequires:  systemd-rpm-macros
Requires:       pcsc-lite

%description
Creates a virtual USB-HID FIDO2 authenticator that relays CTAP2 to a PC/SC
smartcard, letting browsers use contact/NFC FIDO2 cards for WebAuthn.
Advertises the Token2 USB vendor ID so smartcard-based keys are recognised.

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
udevadm trigger || :
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
* Mon Jul 13 2026 Token2 <support@token2.com> - 0.1.0-1
- Initial C++ port with configurable VID/PID (default Token2 349e:0001).
