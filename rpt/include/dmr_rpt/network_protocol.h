// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "dmr_rpt/config.h"

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dmr_rpt {

enum class RelaySource {
    Dmr,
    Fm,
};

struct NetworkChannelPatch {
    std::optional<std::int64_t> rx_frequency_hz;
    std::optional<std::int64_t> tx_frequency_hz;
    std::optional<std::int32_t> rx_gain_tenths_db;
    std::optional<std::int32_t> tx_gain_tenths_db;
    std::optional<bool> fm_enabled;
    std::optional<std::int32_t> ctcss_tone_tenths_hz;
};

struct NetworkControlCommand {
    std::string request_id;
    std::string operation;
    std::string profile_id;
    std::optional<bool> persist_active_profile;
    std::string gain_mode;
    std::optional<int> listen_port;
    std::optional<int> calibration_rx_channel;
    std::string calibration_band;
    std::string calibration_session_id;
    std::optional<int> calibration_input_dbm;
    std::optional<std::int32_t> calibration_rx_gain_tenths_db;
    std::optional<std::int32_t> calibration_start_input_dbm;
    NetworkChannelPatch channel_patch;
};

struct NetworkControlResult {
    bool accepted = false;
    std::string code = "internal_error";
    std::string message;
    std::string state_json;
};

struct NetworkRelayEvent {
    std::string event;
    RelaySource source = RelaySource::Dmr;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    int slot = 1;
    int color_code = 1;
    std::int64_t relay_duration_ms = 0;
    std::optional<double> average_rssi_dbfs;
    std::string correlation_id;
};

struct NetworkReceiveCall {
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    std::string mode;
};

struct ReceiveStatus {
    int rx_channel = 0;
    std::string receiver_mode = "idle";
    bool receiving = false;
    std::optional<double> rssi_dbfs;
    std::optional<double> snr_db;
    std::optional<NetworkReceiveCall> active_call;
    bool active_call_state_known = false;
    std::optional<double> rssi_dbm;
    std::string calibration_state = "uncalibrated";
};

class NetworkEventSink {
public:
    virtual ~NetworkEventSink() = default;
    virtual void observe_signal(int rx_channel, double average_power) = 0;
    virtual void observe_receive_status(const ReceiveStatus&) {}
    virtual void publish_relay_event(const NetworkRelayEvent& event) = 0;
};

struct NetworkControlCallbacks {
    std::function<NetworkControlResult(const NetworkControlCommand&)> command;
    std::function<std::string()> status_json;
    std::function<std::string()> gain_control_json;
};

class TcpStatusPublisher final : public NetworkEventSink {
public:
    TcpStatusPublisher(TcpStatusConfig config,
                       std::string device_id,
                       NetworkControlCallbacks callbacks = {});
    ~TcpStatusPublisher();

    TcpStatusPublisher(const TcpStatusPublisher&) = delete;
    TcpStatusPublisher& operator=(const TcpStatusPublisher&) = delete;

    void start();
    void stop() noexcept;
    bool running() const;

    void observe_signal(int rx_channel, double average_power) override;
    void publish_relay_event(const NetworkRelayEvent& event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class UdpControlServer final {
public:
    UdpControlServer(UdpControlConfig config,
                     NetworkControlCallbacks callbacks = {});
    ~UdpControlServer();

    UdpControlServer(const UdpControlServer&) = delete;
    UdpControlServer& operator=(const UdpControlServer&) = delete;

    void start();
    void stop() noexcept;
    bool running() const;

    void observe_receive_status(const ReceiveStatus& status);
    void observe_relay_event(const NetworkRelayEvent& event);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NetworkControlService final : public NetworkEventSink {
public:
    NetworkControlService(UdpControlConfig udp_config,
                          TcpStatusConfig tcp_config,
                          std::string device_id,
                          NetworkControlCallbacks callbacks = {});
    ~NetworkControlService();

    NetworkControlService(const NetworkControlService&) = delete;
    NetworkControlService& operator=(const NetworkControlService&) = delete;

    void start();
    void stop() noexcept;
    bool running() const;

    void observe_signal(int rx_channel, double average_power) override;
    void observe_receive_status(const ReceiveStatus& status) override;
    void publish_relay_event(const NetworkRelayEvent& event) override;

private:
    std::shared_ptr<TcpStatusPublisher> tcp_;
    std::unique_ptr<UdpControlServer> udp_;
};

const char* to_string(RelaySource source);

} // namespace dmr_rpt
