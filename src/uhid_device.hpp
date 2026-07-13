// uhid_device.hpp — thin wrapper over the Linux /dev/uhid character device.
//
// Replaces the python `uhid` package. We issue UHID_CREATE2 with the same
// FIDO report descriptor and the Token2 VID/PID, then run a blocking read
// loop dispatching UHID_OUTPUT (host->device) events to a callback and
// sending UHID_INPUT2 (device->host) frames back.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/uhid.h>
#include <unistd.h>

namespace fido2bridge {

using Bytes = std::vector<uint8_t>;

// Default identity: Token2 VID, PID 0x0010. Overridable via constructor so the
// shipped build can use a dedicated virtual PID without recompiling callers.
// Compile-time overridable so packaging can flip the shipped default in one
// place (e.g. -DBRIDGE_DEFAULT_PID=0x0200) without editing source.
#ifndef BRIDGE_DEFAULT_VID
#define BRIDGE_DEFAULT_VID 0x349E  // Token2
#endif
#ifndef BRIDGE_DEFAULT_PID
#define BRIDGE_DEFAULT_PID 0x0001
#endif
static constexpr uint16_t DEFAULT_VID = BRIDGE_DEFAULT_VID;
static constexpr uint16_t DEFAULT_PID = BRIDGE_DEFAULT_PID;

// FIDO CTAPHID report descriptor — identical bytes to the Python bridge.
static const std::vector<uint8_t> FIDO_REPORT_DESCRIPTOR = {
    0x06, 0xD0, 0xF1,  // Usage Page (FIDO)
    0x09, 0x01,        // Usage (CTAPHID)
    0xA1, 0x01,        // Collection (Application)
    0x09, 0x20,        // Usage (Data In)
    0x15, 0x00,        // Logical min (0)
    0x26, 0xFF, 0x00,  // Logical max (255)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x40,        // Report count (64)
    0x81, 0x02,        // Input (Data,Var,Abs)
    0x09, 0x21,        // Usage (Data Out)
    0x15, 0x00,        // Logical min (0)
    0x26, 0xFF, 0x00,  // Logical max (255)
    0x75, 0x08,        // Report Size (8)
    0x95, 0x40,        // Report count (64)
    0x91, 0x02,        // Output (Data,Var,Abs)
    0xC0,              // End Collection
};

class UHidDevice {
public:
    // Callbacks mirror the python receive_output / receive_open / receive_close.
    std::function<void(const Bytes&)> on_output;
    std::function<void()>             on_open;
    std::function<void()>             on_close;

    UHidDevice(uint16_t vid = DEFAULT_VID, uint16_t pid = DEFAULT_PID,
               std::string name = "FIDO2 Virtual USB Device")
        : vid_(vid), pid_(pid), name_(std::move(name)) {
        fd_ = ::open("/dev/uhid", O_RDWR | O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error(
                "open(/dev/uhid) failed — is the uhid module loaded and are we "
                "root? errno=" + std::to_string(errno));
        create();
    }

    ~UHidDevice() {
        if (fd_ >= 0) {
            struct uhid_event ev{};
            ev.type = UHID_DESTROY;
            (void)write_event(ev);
            ::close(fd_);
        }
    }

    UHidDevice(const UHidDevice&) = delete;
    UHidDevice& operator=(const UHidDevice&) = delete;

    // Send a 64-byte HID input report (device -> host).
    void send_input(const Bytes& report) {
        struct uhid_event ev{};
        ev.type = UHID_INPUT2;
        size_t n = report.size();
        if (n > sizeof(ev.u.input2.data)) n = sizeof(ev.u.input2.data);
        ev.u.input2.size = static_cast<uint16_t>(n);
        std::memcpy(ev.u.input2.data, report.data(), n);
        write_event(ev);
    }

    // Blocking read loop. Runs until the device is torn down / read error.
    void run() {
        for (;;) {
            struct uhid_event ev{};
            ssize_t ret = ::read(fd_, &ev, sizeof(ev));
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;  // device gone
            }
            if (ret == 0) break;
            dispatch(ev);
        }
    }

private:
    int      fd_ = -1;
    uint16_t vid_, pid_;
    std::string name_;

    void create() {
        struct uhid_event ev{};
        ev.type = UHID_CREATE2;

        std::strncpy(reinterpret_cast<char*>(ev.u.create2.name), name_.c_str(),
                     sizeof(ev.u.create2.name) - 1);

        std::memcpy(ev.u.create2.rd_data, FIDO_REPORT_DESCRIPTOR.data(),
                    FIDO_REPORT_DESCRIPTOR.size());
        ev.u.create2.rd_size =
            static_cast<uint16_t>(FIDO_REPORT_DESCRIPTOR.size());

        ev.u.create2.bus     = BUS_USB;
        ev.u.create2.vendor  = vid_;
        ev.u.create2.product = pid_;
        ev.u.create2.version = 0;
        ev.u.create2.country = 0;

        write_event(ev);
    }

    void dispatch(const struct uhid_event& ev) {
        switch (ev.type) {
            case UHID_START:
                // Device is live; nothing required here.
                break;
            case UHID_OPEN:
                if (on_open) on_open();
                break;
            case UHID_CLOSE:
                if (on_close) on_close();
                break;
            case UHID_OUTPUT: {
                // Host wrote a report to us. The first byte is the report id
                // (0 here); python-uhid passes the full buffer through, so we
                // do the same and let the CTAP-HID layer index from byte 0.
                const auto& o = ev.u.output;
                Bytes buf(o.data, o.data + o.size);
                if (on_output) on_output(buf);
                break;
            }
            default:
                break;
        }
    }

    bool write_event(const struct uhid_event& ev) {
        ssize_t ret = ::write(fd_, &ev, sizeof(ev));
        return ret == static_cast<ssize_t>(sizeof(ev));
    }
};

}  // namespace fido2bridge
