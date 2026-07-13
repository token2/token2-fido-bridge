// pcsc_manager.hpp — establishes a PC/SC context and hands out PcscDevices.
//
// Mirrors get_pcsc_device(): polls for up to SECONDS_TO_WAIT_FOR_AUTHENTICATOR
// for a reader with a present card, connects, and wraps it in a PcscDevice.
#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "pcsc.hpp"

namespace fido2bridge {

static constexpr int SECONDS_TO_WAIT_FOR_AUTHENTICATOR = 10;

class PcscManager {
public:
    PcscManager() {
        LONG rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr,
                                        &ctx_);
        if (rv != SCARD_S_SUCCESS)
            throw PcscError("SCardEstablishContext failed");
    }

    ~PcscManager() {
        if (ctx_) SCardReleaseContext(ctx_);
    }

    PcscManager(const PcscManager&) = delete;
    PcscManager& operator=(const PcscManager&) = delete;

    // Poll for a card and return a connected device, or nullptr on timeout.
    std::unique_ptr<PcscDevice> wait_for_device() {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() +
            std::chrono::seconds(SECONDS_TO_WAIT_FOR_AUTHENTICATOR);

        while (clock::now() < deadline) {
            auto readers = list_readers();
            for (const auto& reader : readers) {
                auto dev = try_connect(reader);
                if (dev) return dev;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return nullptr;
    }

private:
    SCARDCONTEXT ctx_ = 0;

    std::vector<std::string> list_readers() {
        DWORD len = 0;
        LONG rv = SCardListReaders(ctx_, nullptr, nullptr, &len);
        if (rv != SCARD_S_SUCCESS || len == 0) return {};

        std::vector<char> buf(len);
        rv = SCardListReaders(ctx_, nullptr, buf.data(), &len);
        if (rv != SCARD_S_SUCCESS) return {};

        // Multi-string: reader names separated by NUL, terminated by double NUL.
        std::vector<std::string> readers;
        const char* p = buf.data();
        while (*p) {
            readers.emplace_back(p);
            p += readers.back().size() + 1;
        }
        return readers;
    }

    std::unique_ptr<PcscDevice> try_connect(const std::string& reader) {
        SCARDHANDLE card = 0;
        DWORD active_protocol = 0;
        LONG rv = SCardConnect(ctx_, reader.c_str(), SCARD_SHARE_SHARED,
                               SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                               &card, &active_protocol);
        if (rv != SCARD_S_SUCCESS) return nullptr;

        try {
            return std::make_unique<PcscDevice>(ctx_, card, active_protocol,
                                                reader);
        } catch (const std::exception&) {
            SCardDisconnect(card, SCARD_LEAVE_CARD);
            return nullptr;
        }
    }
};

}  // namespace fido2bridge
