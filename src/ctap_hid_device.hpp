// ctap_hid_device.hpp — CTAP-HID transport state machine.
//
// Faithful port of fido2_hid_bridge/ctap_hid_device.py. Handles:
//   - init vs continuation packet parsing (0x80 command bit)
//   - per-channel receive buffers with sequence checking and idle cleanup
//   - INIT channel assignment, CBOR/MSG/PING/CANCEL/WINK/KEEPALIVE handlers
//   - 64-byte response framing (7-byte init header, 5-byte cont header)
// The PC/SC side is delegated to PcscManager / PcscDevice.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pcsc.hpp"
#include "pcsc_manager.hpp"
#include "uhid_device.hpp"

namespace fido2bridge {

static constexpr double INACTIVITY_CLEANUP_SECONDS   = 30.0;
static constexpr int    MAX_SIMULTANEOUS_CONNECTIONS = 100;
static constexpr size_t PACKET_SIZE                  = 64;

static const Bytes BROADCAST_CHANNEL = {0xFF, 0xFF, 0xFF, 0xFF};

class CtapHidDevice {
public:
    CtapHidDevice(uint16_t vid = DEFAULT_VID, uint16_t pid = DEFAULT_PID,
                  std::string name = "FIDO2 Virtual USB Device")
        : uhid_(vid, pid, std::move(name)), rng_(std::random_device{}()) {
        uhid_.on_output = [this](const Bytes& b) { process_hid_message(b); };
        uhid_.on_open   = [this]() { process_open(); };
        uhid_.on_close  = [this]() { process_close(); };
    }

    void run() { uhid_.run(); }

private:
    // Per-channel receive state, mirroring the python tuple:
    // (cmd, total_len, last_seq, accumulated_data, last_activity_time)
    struct ChannelState {
        CtapHid cmd;
        int     total_len;
        int     last_seq;
        Bytes   data;
        double  last_activity;
    };

    UHidDevice                     uhid_;
    PcscManager                    pcsc_;
    std::unique_ptr<PcscDevice>    chosen_device_;
    std::map<std::string, ChannelState> channels_;
    int                            reference_count_ = 0;
    std::mt19937                   rng_;

    static double now() {
        using namespace std::chrono;
        return duration_cast<duration<double>>(
                   steady_clock::now().time_since_epoch())
            .count();
    }

    static std::string channel_key(const Bytes& channel) {
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", channel[0],
                      channel[1], channel[2], channel[3]);
        return buf;
    }

    void process_open() { reference_count_++; }

    void process_close() {
        if (reference_count_ > 0) reference_count_--;
        if (reference_count_ == 0) {
            channels_.clear();
            close_pcsc();
        }
    }

    void close_pcsc() { chosen_device_.reset(); }

    // ---- incoming packet handling ----------------------------------------

    void process_hid_message(const Bytes& buffer) {
        if (buffer.size() < 8) return;  // malformed; ignore

        double t = now();
        // Reap idle channels.
        for (auto it = channels_.begin(); it != channels_.end();) {
            if (t - it->second.last_activity >= INACTIVITY_CLEANUP_SECONDS)
                it = channels_.erase(it);
            else
                ++it;
        }

        if (is_initial_packet(buffer)) {
            Bytes channel(buffer.begin() + 1, buffer.begin() + 5);
            uint8_t cmd_byte = buffer[5] & 0x7F;
            int lc = (static_cast<int>(buffer[6]) << 8) + buffer[7];

            CtapHid cmd;
            if (!valid_command(cmd_byte, cmd)) {
                send_error(channel, 0x01);
                return;
            }
            int avail = static_cast<int>(buffer.size()) - 8;
            int take = lc < avail ? lc : avail;
            Bytes data(buffer.begin() + 8, buffer.begin() + 8 + take);

            channels_[channel_key(channel)] =
                ChannelState{cmd, lc, -1, data, t};
            if (lc == static_cast<int>(data.size())) finish_receiving(channel);
        } else {
            Bytes channel(buffer.begin() + 1, buffer.begin() + 5);
            uint8_t seq = buffer[5];
            Bytes new_data(buffer.begin() + 6, buffer.end());

            std::string key = channel_key(channel);
            auto it = channels_.find(key);
            if (it == channels_.end()) {
                send_error(channel, 0x0B);  // ERR_INVALID_CHANNEL
                return;
            }
            ChannelState& st = it->second;
            if (seq != st.last_seq + 1) {
                handle_cancel(channel);
                send_error(channel, 0x04);  // ERR_INVALID_SEQ
                return;
            }
            int remaining = st.total_len - static_cast<int>(st.data.size());
            int take = static_cast<int>(new_data.size());
            if (take > remaining) take = remaining;
            st.data.insert(st.data.end(), new_data.begin(),
                           new_data.begin() + take);
            st.last_seq = seq;
            st.last_activity = t;
            if (st.total_len == static_cast<int>(st.data.size()))
                finish_receiving(channel);
        }
    }

    static bool is_initial_packet(const Bytes& buffer) {
        return (buffer[5] & 0x80) != 0;
    }

    static bool valid_command(uint8_t b, CtapHid& out) {
        switch (b) {
            case 0x01: out = CtapHid::PING;      return true;
            case 0x03: out = CtapHid::MSG;       return true;
            case 0x06: out = CtapHid::INIT;      return true;
            case 0x08: out = CtapHid::WINK;      return true;
            case 0x10: out = CtapHid::CBOR;      return true;
            case 0x11: out = CtapHid::CANCEL;    return true;
            case 0x3B: out = CtapHid::KEEPALIVE; return true;
            case 0x3F: out = CtapHid::ERROR;     return true;
            default:                             return false;
        }
    }

    // ---- command dispatch -------------------------------------------------

    void finish_receiving(const Bytes& channel) {
        std::string key = channel_key(channel);
        auto it = channels_.find(key);
        if (it == channels_.end()) return;
        CtapHid cmd = it->second.cmd;
        Bytes data = it->second.data;
        handle_cancel(channel);  // clears the channel state

        Bytes response;
        bool have_response = true;
        try {
            switch (cmd) {
                case CtapHid::INIT:
                    if (!handle_init(channel, data, response))
                        return;  // already handled (error sent)
                    break;
                case CtapHid::CBOR:
                    if (!handle_cbor(channel, data, response)) return;
                    break;
                case CtapHid::MSG:
                    if (!handle_msg(channel, data, response)) return;
                    break;
                case CtapHid::PING:
                    response = data;  // echo
                    break;
                case CtapHid::CANCEL:
                    response = Bytes{};
                    break;
                case CtapHid::WINK:
                    response = Bytes{};  // no-op over PC/SC
                    break;
                case CtapHid::KEEPALIVE:
                    response = Bytes{1};
                    break;
                default:
                    send_error(channel, 0x01);
                    return;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "Error: %s\n", e.what());
            send_error(channel, 0x7F);  // ERR_OTHER
            close_pcsc();
            return;
        }
        if (!have_response) return;

        for (const auto& pkt : encode_response_packets(channel, cmd, response))
            uhid_.send_input(pkt);
    }

    // INIT: assign a channel (broadcast) or re-init existing, return nonce echo
    // + new channel id + version/capability block. Returns false if it already
    // sent an error / couldn't get a device.
    bool handle_init(const Bytes& channel, const Bytes& buffer, Bytes& out) {
        if (buffer.size() != 8) {
            send_error(BROADCAST_CHANNEL, 0x03);  // ERR_INVALID_LEN
            return false;
        }

        Bytes new_channel;
        PcscDevice* dev = nullptr;

        if (channel == BROADCAST_CHANNEL) {
            if (channels_.size() > MAX_SIMULTANEOUS_CONNECTIONS) {
                send_error(channel, 0x06);
                return false;
            }
            new_channel = assign_channel_id();
            dev = get_pcsc_device();
            if (!dev) return false;
        } else {
            handle_cancel(channel);
            dev = get_pcsc_device();
            if (!dev) return false;
            new_channel = channel;
        }

        out.clear();
        out.insert(out.end(), buffer.begin(), buffer.end());        // nonce
        out.insert(out.end(), new_channel.begin(), new_channel.end());
        out.push_back(0x02);                 // CTAPHID protocol version
        out.push_back(0x01);                 // device version major
        out.push_back(0x00);                 // device version minor
        out.push_back(0x00);                 // device version build
        out.push_back(dev->capabilities());  // capabilities from the card
        return true;
    }

    bool handle_cbor(const Bytes& channel, const Bytes& buffer, Bytes& out) {
        (void)channel;
        PcscDevice* dev = get_pcsc_device();
        if (!dev) return false;
        try {
            out = dev->call(CtapHid::CBOR, buffer);
        } catch (const CtapError& e) {
            out = Bytes{e.code};  // return the CTAP status byte as the body
        }
        return true;
    }

    bool handle_msg(const Bytes& channel, const Bytes& buffer, Bytes& out) {
        (void)channel;
        PcscDevice* dev = get_pcsc_device();
        if (!dev) return false;
        out = dev->call(CtapHid::MSG, buffer);
        return true;
    }

    void handle_cancel(const Bytes& channel) {
        channels_.erase(channel_key(channel));
    }

    // ---- device acquisition ----------------------------------------------

    PcscDevice* get_pcsc_device() {
        if (!chosen_device_) {
            chosen_device_ = pcsc_.wait_for_device();
            if (!chosen_device_) {
                // python raises; we surface as a CTAP error upstream.
                throw std::runtime_error(
                    "Could not connect to a PC/SC device in time!");
            }
        }
        return chosen_device_.get();
    }

    // ---- channel id assignment -------------------------------------------

    Bytes assign_channel_id() {
        std::uniform_int_distribution<int> dist(0, 255);
        for (int attempt = 0; attempt < 10; ++attempt) {
            Bytes cid = {
                static_cast<uint8_t>(dist(rng_)),
                static_cast<uint8_t>(dist(rng_)),
                static_cast<uint8_t>(dist(rng_)),
                static_cast<uint8_t>(dist(rng_)),
            };
            if (cid == Bytes{0, 0, 0, 0} || cid == BROADCAST_CHANNEL) continue;
            if (channels_.count(channel_key(cid))) continue;
            return cid;
        }
        throw std::runtime_error("Unable to assign an unused channel ID!");
    }

    // ---- response framing -------------------------------------------------

    std::vector<Bytes> encode_response_packets(const Bytes& channel,
                                               CtapHid cmd, const Bytes& data) {
        std::vector<Bytes> out;
        size_t offset = 0;
        int seq = 0;
        for (;;) {
            Bytes response;
            size_t capacity;
            if (seq == 0) {
                capacity = PACKET_SIZE - 7;
                size_t take =
                    (offset + capacity <= data.size()) ? capacity
                                                        : data.size() - offset;
                response.insert(response.end(), channel.begin(), channel.end());
                response.push_back(static_cast<uint8_t>(cmd) | 0x80);
                response.push_back(static_cast<uint8_t>(data.size() >> 8));
                response.push_back(static_cast<uint8_t>(data.size() & 0xFF));
                response.insert(response.end(), data.begin() + offset,
                                data.begin() + offset + take);
            } else {
                capacity = PACKET_SIZE - 5;
                size_t take =
                    (offset + capacity <= data.size()) ? capacity
                                                        : data.size() - offset;
                response.insert(response.end(), channel.begin(), channel.end());
                response.push_back(static_cast<uint8_t>(seq - 1));
                response.insert(response.end(), data.begin() + offset,
                                data.begin() + offset + take);
            }
            if (response.size() < PACKET_SIZE)
                response.resize(PACKET_SIZE, 0x00);  // pad
            out.push_back(std::move(response));

            offset += capacity;
            seq++;
            if (offset >= data.size()) break;
        }
        return out;
    }

    void send_error(const Bytes& channel, uint8_t error_type) {
        for (const auto& pkt : encode_response_packets(channel, CtapHid::ERROR,
                                                       Bytes{error_type}))
            uhid_.send_input(pkt);
    }
};

}  // namespace fido2bridge
