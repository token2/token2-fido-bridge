// main.cpp — entry point for the C++ FIDO2 HID<->PC/SC bridge.
//
// Watches PC/SC readers and exposes a virtual UHID FIDO device (Token2
// 349e:0001 by default) ONLY while a FIDO card is present — like plugging in a
// physical key. With no card there is no virtual device, so OpenSSH sk keys,
// libfido2 and pam-u2f see only real authenticators (issue #2).
//
// VID/PID/name are configurable so the shipped build can use a dedicated
// virtual PID without editing source (see --help).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "ctap_hid_device.hpp"
#include "pcsc_manager.hpp"
#include "uhid_device.hpp"

using namespace fido2bridge;

static void usage(const char* prog) {
    std::printf(
        "Usage: %s [--vid HEX] [--pid HEX] [--name NAME]\n"
        "\n"
        "  --vid HEX    USB vendor ID  (default 0x%04X, Token2)\n"
        "  --pid HEX    USB product ID (default 0x%04X)\n"
        "  --name NAME  device name    (default \"FIDO2 Virtual USB Device\")\n"
        "  -h, --help   show this help\n"
        "\n"
        "Environment overrides (used when the matching flag is absent):\n"
        "  FIDO2_BRIDGE_VID, FIDO2_BRIDGE_PID, FIDO2_BRIDGE_NAME\n",
        prog, DEFAULT_VID, DEFAULT_PID);
}

static bool parse_hex16(const char* s, uint16_t& out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 16);
    if (*end != '\0' || v < 0 || v > 0xFFFF) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

int main(int argc, char** argv) {
    uint16_t vid = DEFAULT_VID, pid = DEFAULT_PID;
    std::string name = "FIDO2 Virtual USB Device";

    // Environment defaults first; flags override.
    if (const char* e = std::getenv("FIDO2_BRIDGE_VID")) parse_hex16(e, vid);
    if (const char* e = std::getenv("FIDO2_BRIDGE_PID")) parse_hex16(e, pid);
    if (const char* e = std::getenv("FIDO2_BRIDGE_NAME")) name = e;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "--vid") {
            if (!parse_hex16(next("--vid"), vid)) {
                std::fprintf(stderr, "invalid --vid\n"); return 2;
            }
        } else if (a == "--pid") {
            if (!parse_hex16(next("--pid"), pid)) {
                std::fprintf(stderr, "invalid --pid\n"); return 2;
            }
        } else if (a == "--name") {
            name = next("--name");
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    std::printf("token2-fido-bridge (C++) starting: vid=0x%04X pid=0x%04X name=\"%s\"\n",
                vid, pid, name.c_str());
    std::fflush(stdout);

    // The virtual device is now created lazily (when a card appears), so check
    // /dev/uhid up front to keep failing fast on a misconfigured system.
    if (::access("/dev/uhid", R_OK | W_OK) != 0) {
        std::fprintf(stderr,
                     "fatal: /dev/uhid not accessible - is the uhid module "
                     "loaded and are we root?\n");
        return 1;
    }

    using clock = std::chrono::steady_clock;
    constexpr auto CARD_POLL     = std::chrono::milliseconds(200);
    constexpr int  HID_POLL_MS   = 200;

    try {
        PcscManager pcsc;
        std::unique_ptr<CtapHidDevice> bridge;
        auto next_presence_check = clock::now();

        for (;;) {
            if (!bridge) {
                // No card: no virtual device. Poll readers cheaply.
                if (auto card = pcsc.find_fido_card()) {
                    std::string reader = card->name();
                    // Throws if /dev/uhid can't be opened -> fatal, as before.
                    bridge = std::make_unique<CtapHidDevice>(std::move(card),
                                                             vid, pid, name);
                    std::printf("FIDO card detected in \"%s\" - virtual device created\n",
                                reader.c_str());
                    std::fflush(stdout);
                    next_presence_check = clock::now() + CARD_POLL;
                } else {
                    std::this_thread::sleep_for(CARD_POLL);
                }
                continue;
            }

            // Card present: service HID traffic, periodically re-check card.
            bool ok = bridge->pump(HID_POLL_MS);
            bool gone = !ok || bridge->failed();
            if (!gone && clock::now() >= next_presence_check) {
                gone = !pcsc.card_present(bridge->reader_name());
                next_presence_check = clock::now() + CARD_POLL;
            }
            if (gone) {
                std::printf("FIDO card removed from \"%s\" - virtual device destroyed\n",
                            bridge->reader_name().c_str());
                std::fflush(stdout);
                bridge.reset();  // UHID_DESTROY: hosts see the key unplugged
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
