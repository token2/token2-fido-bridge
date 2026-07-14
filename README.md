# token2-fido-bridge for Linux

<img width="200"  alt="image" src="https://github.com/user-attachments/assets/e2b0feba-587b-4f60-b686-c6f3a018fa8e" align=right />

A lightweight native daemon that lets browsers use **PC/SC smartcards** (contact
or NFC) as **FIDO2 / WebAuthn** security keys on Linux. 

This project is **[different from FIDO Bridge for Android](https://www.token2.swiss/site/page/fido-bridge-for-android-user-manual)**, but serves a similar
purpose: bridging smartcard-based credentials to FIDO2 / WebAuthn. 

This version  is designed specifically for **Linux desktop environments**. Inspired by an earlier Python [implementation](https://github.com/BryanJacobs/fido2-hid-bridge) of the same idea; this is an
independent **C++** rewrite focused on a single static binary, minimal
footprint, and clean native packaging.



## About

`token2-fido-bridge` is developed and maintained by [Token2](https://www.token2.swiss).
While it is built and tested with Token2 FIDO2.1 security keys and smartcards, it is
not tied to Token2 hardware in any way : it works with **any FIDO2 / FIDO2.1
authenticator** exposed over PC/SC, including contact smartcards, NFC tokens, and
NFC-capable security keys from other vendors. The bridge speaks the standard CTAP2
and ISO 7816 protocols, so any spec-compliant device that your PC/SC stack can see
should work.



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

Test at https://www.token2.swiss/tools/fido2-demo with a card in the reader.

## Current support status

As of now, the project has only been tested on **Ubuntu**.

Tested with **Chromium** and the **Snap version of Firefox** included with
Ubuntu. Snap-confined Firefox is supported through a udev rule (installed
automatically by the package) that tags the virtual device for the browser's
sandbox.

### Firefox (Snap) — one-time restart

Snap-packaged Firefox (the default on Ubuntu) decides which devices it can
access **at launch**. If Firefox is already running when you install
token2-fido-bridge, it won't see the virtual key until you fully restart it —
closing the window is not enough, because Snap keeps a background process.

Fully quit and reopen Firefox:

    snap stop firefox
    pkill -f firefox

Then reopen Firefox and try again at https://www.token2.swiss/tools/fido2-demo  

If Firefox still doesn't detect the key, make sure the u2f interface is
connected:

    snap connect firefox:u2f-devices

Chromium and Chrome pick up the device immediately and need no restart.

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
