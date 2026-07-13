# token2-fido-bridge for Linux <img width="200"  alt="image" src="https://github.com/user-attachments/assets/f1c51519-14fb-4888-be3a-c89ca551319e" align=right />

A lightweight native daemon that lets browsers use **PC/SC smartcards** (contact
or NFC) as **FIDO2 / WebAuthn** security keys on Linux. 

This project is **[different from FIDO Bridge for Android](https://www.token2.swiss/site/page/fido-bridge-for-android-user-manual)**, but serves a similar
purpose: bridging smartcard-based credentials to FIDO2 / WebAuthn. 

This version  is designed specifically for **Linux desktop environments**.

## Current support status

As of now, the project has only been tested on **Ubuntu**.

Browser support has currently been tested with a **regular installation of
Chromium**. The **Snap version of Firefox included with Ubuntu is not yet
supported**.


It creates a virtual USB-HID FIDO2 authenticator through the Linux `uhid`
kernel interface and relays CTAP2 / U2F traffic to a smartcard over PC/SC — so
any browser that speaks USB-HID WebAuthn can authenticate with a card-based
credential, no special browser support required.

Inspired by an earlier Python [implementation](https://github.com/BryanJacobs/fido2-hid-bridge) of the same idea; this is an
independent **C++** rewrite focused on a single static binary, minimal
footprint, and clean native packaging.

## Why C++

Written in C++, `token2-fido-bridge` ships as a **single native binary of a few
hundred kilobytes** with essentially no runtime dependencies beyond the system
PC/SC library. Comparable Python-based bridges pull in a Python interpreter plus
their dependency tree — typically **15 MB or more** on disk, a virtualenv to
manage, and an interpreter that can break across Python version upgrades. This
project has none of that: install the `.deb`, and it just runs.

| | This project (C++) | Typical Python bridge |
|---|---|---|
| Binary / install size | ~250 KB | ~15 MB+ |
| Runtime RAM | ~200 KB | ~15 MB |
| Dependencies | system PC/SC only | Python + pip packages |
| Interpreter | none | required |

## Features

- Single self-contained binary (~250 KB), ~200 KB RAM at runtime
- No Python, no virtualenv, no runtime interpreter
- Faithful CTAP-HID transport and CTAP2-over-ISO7816 APDU handling
- Configurable USB VID/PID (flags, env vars, or build-time defaults)
- Ships a systemd service, udev rule, and `uhid` module auto-load
- `.deb`, `.rpm`, and one-line installer

## Requirements

- Linux with the `uhid` kernel module (standard on Ubuntu/Debian/Fedora)
- `pcscd` running, a PC/SC reader, and a FIDO2-capable smartcard
- Runs as root (needs `/dev/uhid`)

## Install

### Debian / Ubuntu / Mint

```sh
sudo apt install ./token2-fido-bridge_0.1.0-1_amd64.deb
```

The package loads the `uhid` module, installs the udev rule, and enables the
service automatically.

### Fedora / openSUSE

```sh
sudo dnf install ./token2-fido-bridge-0.1.0-1.x86_64.rpm
```

### One-line installer (builds from source if no prebuilt package is hosted)

```sh
curl -sSL https://example.com/install.sh | sudo sh
```

## Build from source

```sh
sudo apt install -y build-essential cmake libpcsclite-dev pcscd   # Debian/Ubuntu
# or: sudo dnf install gcc-c++ cmake pcsc-lite-devel pcsc-lite     # Fedora

cmake -B build -S .
cmake --build build -j"$(nproc)"
sudo cmake --install build
```

### Build a `.deb`

```sh
sudo apt install -y devscripts debhelper dpkg-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../token2-fido-bridge_0.1.0-1_*.deb
```

### Build an `.rpm`

```sh
rpmbuild -ta token2-fido-bridge-0.1.0.tar.gz
```

## Usage

Once installed the service runs automatically. To run manually:

```sh
sudo token2-fido-bridge                 # defaults
sudo token2-fido-bridge --vid 349e --pid 0001 --name "My Virtual Key"
```

Options:

| Flag | Env var | Description |
|------|---------|-------------|
| `--vid HEX` | `FIDO2_BRIDGE_VID` | USB vendor ID (default `0x349E`) |
| `--pid HEX` | `FIDO2_BRIDGE_PID` | USB product ID (default `0x0001`) |
| `--name NAME` | `FIDO2_BRIDGE_NAME` | Device name |

Test at [webauthn.io](https://webauthn.io) with a card in the reader.

## Configuration

### Device identity

The default identity is `349E:0001`. To change it fleet-wide at build time:

```sh
cmake -B build -S . -DBRIDGE_DEFAULT_PID=0x0200
```

If you change the PID, update the `HID_ID` in
`packaging/70-token2-fido-bridge.rules` to match
(`0003:0000VVVV:0000PPPP`, uppercase hex).

## How it works

```
Browser  ──USB-HID/CTAP──▶  /dev/uhid  ──▶  token2-fido-bridge  ──PC/SC──▶  smartcard
   ▲                          (virtual FIDO2 device)                        │
   └──────────────────────── WebAuthn response ◀───────────────────────────┘
```

The daemon presents a virtual FIDO2 HID authenticator to the OS. When a browser
sends CTAP-HID frames, the bridge reassembles them, translates CBOR commands
into ISO 7816 APDUs, and exchanges them with the card via PC/SC — then frames
the card's response back over HID.

## Packaging notes

This is a privileged system daemon: it needs `/dev/uhid`, installs a systemd
service, and drops a udev rule into the system tree. Native packages
(`.deb` / `.rpm`) are the right delivery mechanism — Flatpak and AppImage are
built for sandboxed user-space apps and can't install the udev rule or service
without root steps anyway.

## License

MIT. See [LICENSE](LICENSE).
