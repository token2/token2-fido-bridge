// pcsc.hpp — PC/SC transport + CTAP2-over-ISO7816 framing.
//
// This is a faithful, hand-rolled port of the parts of python-fido2's
// fido2/pcsc.py that token2-fido-bridge relies on. No external CTAP library:
// we talk to libpcsclite directly and build the APDUs ourselves.
//
// The APDU framing below is byte-for-byte what python-fido2 2.2.1 emits:
//   - AID select:      00 A4 04 00 <len> A0 00 00 06 47 2F 00 01
//   - NFCCTAP_MSG:     80 10 <p1> 00 <data...>          (CBOR command)
//   - NFCCTAP_GETRESP: 80 11 <p1> 00                    (keepalive poll)
//   - short-APDU chaining with 0x10 CLA bit for >255B payloads
//   - GET RESPONSE:    00 C0 00 00 <le>   while SW1==0x61
//   - SW_SUCCESS = 90 00, SW_UPDATE = 91 00, SW1_MORE_DATA = 0x61
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <winscard.h>   // provided by libpcsclite-dev; PCSC-Lite headers

namespace fido2bridge {

using Bytes = std::vector<uint8_t>;

// FIDO applet AID.
static const Bytes AID_FIDO = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};

// Status words.
static constexpr uint8_t SW1_MORE_DATA = 0x61;
static constexpr uint16_t SW_SUCCESS   = 0x9000;
static constexpr uint16_t SW_UPDATE    = 0x9100;

// CTAPHID command bytes (mirrors fido2/hid CTAPHID enum).
enum class CtapHid : uint8_t {
    PING      = 0x01,
    MSG       = 0x03,
    INIT      = 0x06,
    WINK      = 0x08,
    CBOR      = 0x10,
    CANCEL    = 0x11,
    KEEPALIVE = 0x3B,
    ERROR     = 0x3F,
};

// CAPABILITY flags (mirrors fido2/hid CAPABILITY IntFlag).
enum Capability : uint8_t {
    CAP_WINK = 0x01,
    CAP_CBOR = 0x04,
    CAP_NMSG = 0x08,
};

// Raised for CTAP-level errors; .code is the CTAP status byte to return.
struct CtapError : std::runtime_error {
    uint8_t code;
    explicit CtapError(uint8_t c)
        : std::runtime_error("CTAP error " + std::to_string(c)), code(c) {}
};

// Raised when PC/SC itself fails (no reader, card removed, etc.).
struct PcscError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// One connected PC/SC authenticator. Construct via PcscManager::wait_for_device.
class PcscDevice {
public:
    PcscDevice(SCARDCONTEXT ctx, SCARDHANDLE card, DWORD active_protocol,
               std::string name)
        : ctx_(ctx), card_(card), protocol_(active_protocol),
          name_(std::move(name)) {
        select_applet();
        // Probe for CTAP2 by issuing authenticatorGetInfo (CBOR cmd 0x04),
        // exactly as python-fido2 does in __init__.
        try {
            call_cbor(Bytes{0x04});
            capabilities_ |= CAP_CBOR;
        } catch (const CtapError&) {
            if (capabilities_ == 0)
                throw PcscError("Unsupported device (no CTAP2)");
        }
    }

    ~PcscDevice() { close(); }

    PcscDevice(const PcscDevice&) = delete;
    PcscDevice& operator=(const PcscDevice&) = delete;

    uint8_t capabilities() const { return capabilities_; }
    const std::string& name() const { return name_; }

    void close() {
        if (card_) {
            SCardDisconnect(card_, SCARD_LEAVE_CARD);
            card_ = 0;
        }
    }

    // Dispatch a CTAPHID command the same way python-fido2's call() does.
    // Returns the raw response body (CBOR for CBOR, APDU resp for MSG).
    Bytes call(CtapHid cmd, const Bytes& data) {
        switch (cmd) {
            case CtapHid::CBOR: return call_cbor(data);
            case CtapHid::MSG:  return call_apdu(data);
            default:
                throw CtapError(0x01);  // ERR_INVALID_COMMAND
        }
    }

private:
    SCARDCONTEXT ctx_;
    SCARDHANDLE  card_;
    DWORD        protocol_;
    std::string  name_;
    uint8_t      capabilities_ = 0;
    bool         use_ext_apdu_ = false;  // bridge cards use short APDUs

    // Low-level transmit of a single APDU. Returns response data and fills sw.
    Bytes transmit(const Bytes& apdu, uint16_t& sw) {
        const SCARD_IO_REQUEST* send_pci =
            (protocol_ == SCARD_PROTOCOL_T1) ? SCARD_PCI_T1 : SCARD_PCI_T0;

        std::vector<uint8_t> recv(4096);
        DWORD recv_len = static_cast<DWORD>(recv.size());

        LONG rv = SCardTransmit(card_, send_pci, apdu.data(),
                                static_cast<DWORD>(apdu.size()), nullptr,
                                recv.data(), &recv_len);
        if (rv != SCARD_S_SUCCESS)
            throw PcscError("SCardTransmit failed: " +
                            pcsc_stringify(rv));
        if (recv_len < 2)
            throw PcscError("APDU response too short");

        sw = static_cast<uint16_t>((recv[recv_len - 2] << 8) | recv[recv_len - 1]);
        return Bytes(recv.begin(), recv.begin() + (recv_len - 2));
    }

    // Short-APDU chaining, faithful to python-fido2 _chain_apdus (non-ext path).
    // cla/ins/p1/p2 + data, splitting into 255-byte blocks with the 0x10 CLA
    // continuation bit, then draining GET RESPONSE while SW1 == 0x61.
    Bytes chain_apdus(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                      const Bytes& data, uint16_t& sw) {
        Bytes resp;
        size_t offset = 0;

        // Send data in <=255-byte chunks. All but the last carry CLA|0x10.
        // Matches the Python while-loop that peels off up to 255 bytes.
        if (data.empty()) {
            Bytes apdu = {cla, ins, p1, p2, 0x00};  // Lc=0 form + Le
            resp = transmit(apdu, sw);
        } else {
            while (offset < data.size()) {
                size_t remaining = data.size() - offset;
                size_t take = remaining > 255 ? 255 : remaining;
                bool last = (offset + take) >= data.size();

                uint8_t chain_cla = last ? cla : (uint8_t)(0x10 | cla);
                Bytes apdu = {chain_cla, ins, p1, p2,
                              static_cast<uint8_t>(take)};
                apdu.insert(apdu.end(), data.begin() + offset,
                            data.begin() + offset + take);
                if (last) apdu.push_back(0x00);  // Le byte on final block
                resp = transmit(apdu, sw);
                offset += take;
            }
        }

        // Drain chained response: while SW1 == 0x61, GET RESPONSE (00 C0 00 00 Le).
        while ((sw >> 8) == SW1_MORE_DATA) {
            uint8_t le = static_cast<uint8_t>(sw & 0xFF);
            Bytes get_resp = {0x00, 0xC0, 0x00, 0x00, le};
            uint16_t sw2 = 0;
            Bytes more = transmit(get_resp, sw2);
            resp.insert(resp.end(), more.begin(), more.end());
            sw = sw2;
        }
        return resp;
    }

    void select_applet() {
        Bytes apdu = {0x00, 0xA4, 0x04, 0x00,
                      static_cast<uint8_t>(AID_FIDO.size())};
        apdu.insert(apdu.end(), AID_FIDO.begin(), AID_FIDO.end());
        uint16_t sw = 0;
        Bytes resp = chain_apdus(0x00, 0xA4, 0x04, 0x00, AID_FIDO, sw);
        (void)resp;
        if (sw != SW_SUCCESS)
            throw PcscError("FIDO applet select failed, SW=" + hex16(sw));
    }

    // NFCCTAP_MSG / NFCCTAP_GETRESPONSE loop, faithful to _call_cbor.
    Bytes call_cbor(const Bytes& data) {
        uint16_t sw = 0;
        // NFCCTAP_MSG: 80 10 <p1=0x80> 00 <data>. p1=0x80 =>
        // use_nfcctap_getresponse=True (the python default).
        Bytes resp = chain_apdus(0x80, 0x10, 0x80, 0x00, data, sw);

        // NFCCTAP_GETRESPONSE poll while card says "still working" (91 00).
        while (sw == SW_UPDATE) {
            // resp[0] is the CTAP STATUS/keepalive byte; we don't forward it,
            // matching how the bridge ignores keepalive content.
            uint16_t poll_sw = 0;
            resp = chain_apdus(0x80, 0x11, 0x00, 0x00, Bytes{}, poll_sw);
            sw = poll_sw;
        }

        if (sw != SW_SUCCESS)
            throw CtapError(0x01);  // ERR_OTHER-ish; python maps to ERR.OTHER
        return resp;
    }

    // Raw U2F/CTAP1 MSG path (short APDU, no NFCCTAP wrapping).
    Bytes call_apdu(const Bytes& apdu) {
        // python _call_apdu -> _chained_apdu_exchange parses cla/ins/p1/p2 +
        // short-APDU data, then chains. We reproduce the short-APDU parse.
        if (apdu.size() < 4)
            throw CtapError(0x01);
        uint8_t cla = apdu[0], ins = apdu[1], p1 = apdu[2], p2 = apdu[3];
        Bytes data;
        if (apdu.size() > 5) {
            uint8_t lc = apdu[4];
            size_t end = 5 + lc;
            if (end <= apdu.size())
                data.assign(apdu.begin() + 5, apdu.begin() + end);
        }
        uint16_t sw = 0;
        Bytes resp = chain_apdus(cla, ins, p1, p2, data, sw);
        // Append SW to the response, as U2F callers expect SW in-band.
        resp.push_back(static_cast<uint8_t>(sw >> 8));
        resp.push_back(static_cast<uint8_t>(sw & 0xFF));
        return resp;
    }

    static std::string hex16(uint16_t v) {
        char buf[5];
        std::snprintf(buf, sizeof(buf), "%04X", v);
        return buf;
    }
    static std::string pcsc_stringify(LONG rv) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)rv);
        return buf;
    }
};

}  // namespace fido2bridge
