// pcsc_manager.hpp — owns the PC/SC context and watches readers for FIDO cards.
//
// The bridge used to create its virtual HID device at startup and then block
// (up to 10 s) inside CTAPHID INIT waiting for a card. Any tool that enumerates
// FIDO HID devices (OpenSSH sk keys, libfido2, pam-u2f) was forced to wait that
// out when no card was present — see issue #2.
//
// Now the manager only answers two NON-BLOCKING questions:
//   find_fido_card()   — is there a reader holding a card with a FIDO applet?
//   card_present(name) — is the card we are bridging still in that reader?
// The main loop uses them to create the virtual device only while a FIDO card
// is actually present, exactly like plugging/unplugging a physical key.
#pragma once

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pcsc.hpp"

namespace fido2bridge {

class PcscManager {
public:
    PcscManager() { ensure_context(); }  // non-fatal if pcscd isn't up yet

    ~PcscManager() { release_context(); }

    PcscManager(const PcscManager&) = delete;
    PcscManager& operator=(const PcscManager&) = delete;

    // Probe readers that currently hold a card. Returns a connected device for
    // the first card whose FIDO applet answers, or nullptr. Never waits.
    // Cards that are present but are not FIDO (bank card, PIV-only card, ID
    // card in a built-in reader...) are remembered and not re-probed until
    // they are removed/re-inserted, so we don't hammer them every poll.
    // A card is only written off after PROBE_ATTEMPTS failed probes on the
    // same insertion: a freshly tapped NFC card can reject the first SELECT /
    // GetInfo while it is still powering up (issue #7).
    std::unique_ptr<PcscDevice> find_fido_card() {
        auto states = reader_states();

        // Forget rejections for readers whose card went away or changed.
        for (auto it = rejected_.begin(); it != rejected_.end();) {
            bool keep = false;
            for (const auto& s : states)
                if (s.name == it->first && is_present(s.state) &&
                    event_count(s.state) == it->second.event)
                    keep = true;
            it = keep ? std::next(it) : rejected_.erase(it);
        }

        for (const auto& s : states) {
            if (!is_present(s.state)) continue;
            auto rej = rejected_.find(s.name);
            if (rej != rejected_.end() && rej->second.fails >= PROBE_ATTEMPTS)
                continue;  // definitely not a FIDO card; wait for removal

            ConnectResult r;
            auto dev = try_connect(s.name, r);
            if (dev) return dev;
            if (r == ConnectResult::NotFido) {
                auto& e = rejected_[s.name];  // zero-initialised if new
                e.event = event_count(s.state);
                if (++e.fails >= PROBE_ATTEMPTS)
                    std::fprintf(stderr,
                                 "Card in \"%s\" has no usable FIDO applet; "
                                 "ignoring it until removed\n",
                                 s.name.c_str());
            }
            // Transient failures (sharing violation, race with removal) are
            // simply retried on the next poll.
        }
        return nullptr;
    }

    // Is a card still present in `reader`? Non-blocking.
    bool card_present(const std::string& reader) {
        if (!ctx_) return false;
        SCARD_READERSTATE st{};
        st.szReader = reader.c_str();
        st.dwCurrentState = SCARD_STATE_UNAWARE;
        LONG rv = SCardGetStatusChange(ctx_, 0, &st, 1);
        if (rv != SCARD_S_SUCCESS) {
            if (context_lost(rv)) context_broken_ = true;
            return false;
        }
        return is_present(st.dwEventState);
    }

private:
    enum class ConnectResult { Ok, Transient, NotFido };

    struct ReaderState {
        std::string name;
        DWORD state;
    };

    SCARDCONTEXT ctx_ = 0;
    bool context_broken_ = false;
    static constexpr int PROBE_ATTEMPTS = 5;  // x 200 ms poll = ~1 s warm-up

    struct Rejection {
        DWORD event;   // card event counter of the insertion we probed
        int   fails;   // failed probes so far on that insertion
    };
    std::map<std::string, Rejection> rejected_;  // reader -> probe history

    static bool is_present(DWORD s) {
        return (s & SCARD_STATE_PRESENT) && !(s & SCARD_STATE_MUTE);
    }
    // pcsc-lite keeps a per-reader card event counter in the high word.
    static DWORD event_count(DWORD s) { return s >> 16; }

    static bool context_lost(LONG rv) {
        return rv == SCARD_E_NO_SERVICE || rv == SCARD_E_SERVICE_STOPPED ||
               rv == SCARD_E_INVALID_HANDLE;
    }

    void release_context() {
        if (ctx_) SCardReleaseContext(ctx_);
        ctx_ = 0;
    }

    // (Re)establish the context; survives pcscd restarts / late start.
    bool ensure_context() {
        if (ctx_ && !context_broken_) return true;
        release_context();
        context_broken_ = false;
        LONG rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr,
                                        &ctx_);
        if (rv != SCARD_S_SUCCESS) {
            ctx_ = 0;
            return false;
        }
        return true;
    }

    std::vector<std::string> list_readers() {
        DWORD len = 0;
        LONG rv = SCardListReaders(ctx_, nullptr, nullptr, &len);
        if (rv != SCARD_S_SUCCESS || len == 0) {
            if (context_lost(rv)) context_broken_ = true;
            return {};
        }
        std::vector<char> buf(len);
        rv = SCardListReaders(ctx_, nullptr, buf.data(), &len);
        if (rv != SCARD_S_SUCCESS) {
            if (context_lost(rv)) context_broken_ = true;
            return {};
        }
        // Multi-string: reader names separated by NUL, terminated by double NUL.
        std::vector<std::string> readers;
        const char* p = buf.data();
        while (*p) {
            readers.emplace_back(p);
            p += readers.back().size() + 1;
        }
        return readers;
    }

    // Current state of every reader, without waiting (timeout 0 + UNAWARE).
    std::vector<ReaderState> reader_states() {
        if (!ensure_context()) return {};
        auto names = list_readers();
        if (names.empty()) return {};

        std::vector<SCARD_READERSTATE> st(names.size());
        for (size_t i = 0; i < names.size(); ++i) {
            st[i] = SCARD_READERSTATE{};
            st[i].szReader = names[i].c_str();
            st[i].dwCurrentState = SCARD_STATE_UNAWARE;
        }
        LONG rv = SCardGetStatusChange(ctx_, 0, st.data(),
                                       static_cast<DWORD>(st.size()));
        if (rv != SCARD_S_SUCCESS) {
            if (context_lost(rv)) context_broken_ = true;
            return {};
        }
        std::vector<ReaderState> out;
        for (size_t i = 0; i < names.size(); ++i)
            out.push_back({names[i], st[i].dwEventState});
        return out;
    }

    std::unique_ptr<PcscDevice> try_connect(const std::string& reader,
                                            ConnectResult& result) {
        SCARDHANDLE card = 0;
        DWORD active_protocol = 0;
        LONG rv = SCardConnect(ctx_, reader.c_str(), SCARD_SHARE_SHARED,
                               SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                               &card, &active_protocol);
        if (rv != SCARD_S_SUCCESS) {
            result = ConnectResult::Transient;
            return nullptr;
        }
        try {
            auto dev = std::make_unique<PcscDevice>(ctx_, card,
                                                    active_protocol, reader);
            result = ConnectResult::Ok;
            return dev;
        } catch (const CtapError&) {
            result = ConnectResult::NotFido;
        } catch (const PcscError& e) {
            // Applet select failed => not a FIDO card. A transmit failure
            // means the card left the field mid-probe; retry next poll.
            std::string what = e.what();
            result = (what.rfind("SCardTransmit", 0) == 0)
                         ? ConnectResult::Transient
                         : ConnectResult::NotFido;
        } catch (const std::exception&) {
            result = ConnectResult::Transient;
        }
        SCardDisconnect(card, SCARD_LEAVE_CARD);
        return nullptr;
    }
};

}  // namespace fido2bridge
