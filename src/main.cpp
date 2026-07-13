// main.cpp — entry point for the C++ FIDO2 HID<->PC/SC bridge.
//
// Creates the virtual UHID FIDO device (Token2 349e:0010 by default) and runs
// the blocking CTAP-HID event loop, relaying to a PC/SC authenticator.
//
// VID/PID/name are configurable so the shipped build can use a dedicated
// virtual PID without editing source (see --help).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "ctap_hid_device.hpp"
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

    try {
        // Note: CtapHidDevice owns its own UHidDevice; we pass vid/pid through.
        CtapHidDevice bridge(vid, pid, name);
        bridge.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
