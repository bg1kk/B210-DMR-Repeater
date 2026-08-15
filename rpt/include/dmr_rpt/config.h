// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmr_rpt/event.h"
#include "dmr_rpt/rx_signal_calibration.h"

namespace dmr_rpt {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& message);
};

struct RfEndpointConfig {
    int channel = 0;
    std::int64_t frequency_hz = 0;
    std::int64_t lo_offset_hz = 0;
    std::int32_t gain_tenths_db = 0;
    std::int64_t bandwidth_hz = 0;
    std::string antenna;
};

struct AnalogFmRxConfig {
    int channel = 0;
    std::int32_t gain_tenths_db = 0;
    std::int64_t bandwidth_hz = 0;
    std::string antenna;
};

struct AnalogFmDemodConfig {
    int max_deviation_hz = 2500;
    int audio_bandwidth_hz = 3000;
    int squelch_tenths_dbfs = -740;
};

struct AnalogFmTxContext {
    int slot = 1;
    std::uint32_t source_id = 9999;
    std::uint32_t destination_id = 0xFFFFFFU;
    int color_code = 1;
    CallType call_type = CallType::AllCall;
};

struct CtcssConfig {
    bool required = true;
    int tone_tenths_hz = 1230;
    int minimum_confidence_tenths_db = 120;
    int minimum_detect_ms = 250;
    int release_hold_ms = 200;
};

struct AnalogFmFallbackConfig {
    bool enabled = false;
    AnalogFmRxConfig rx;
    AnalogFmDemodConfig fm;
    CtcssConfig ctcss;
    int dmr_idle_guard_ms = 0;
    AnalogFmTxContext dmr_tx;
};

struct ChannelProfile {
    std::string id;
    RfEndpointConfig dmr_rx;
    RfEndpointConfig dmr_tx;
    AnalogFmFallbackConfig analog_fm_fallback;
};

struct IoPinConfig {
    std::string logical_name;
    int io = -1;
    std::optional<int> rx_channel;
    std::optional<int> tx_channel;
};

struct IoStatusConfig {
    bool enabled = true;
    std::string gpio_bank = "FP0";
    std::string active_level = "low";
    std::string idle_level = "high";
    int rx_release_delay_ms = 500;
    int tx_release_delay_ms = 800;
    std::vector<IoPinConfig> pins;
};

struct ReceiveAgcConfig {
    bool enabled = true;
    std::int32_t target_tenths_dbfs = -200;
    std::int32_t minimum_gain_tenths_db = -300;
    std::int32_t maximum_gain_tenths_db = 300;
    std::int32_t attack_tenths_db_per_second = 6000;
    std::int32_t release_tenths_db_per_second = 1200;
};

struct ReceiveGainControlConfig {
    std::int32_t high_gain_tenths_db = 500;
    std::int32_t medium_gain_tenths_db = 250;
    std::int32_t low_gain_tenths_db = 0;
    std::string default_mode = "auto";
    struct AutomaticSwitchingConfig {
        bool enabled = true;
        int high_to_low_threshold_dbm = -70;
        int low_to_high_threshold_dbm = -60;
    } automatic_switching;
};

struct RxFrontendConditioningConfig {
    bool enabled = false;
    std::string gpio_bank = "FP0";
    int stage_bit0_io = 4;
    int stage_bit1_io = 5;
    int default_stage = 0;
    std::array<double, 4> low_attenuation_db{0.0, 10.0, 20.0, 30.0};
    std::array<double, 4> medium_attenuation_db{0.0, 10.0, 20.0, 30.0};
    std::array<double, 4> high_attenuation_db{0.0, 10.0, 20.0, 30.0};
};

struct RadioConfig {
    std::string uhd_device = "type=b200";
    std::int64_t operating_frequency_min_hz = 136000000;
    std::int64_t operating_frequency_max_hz = 520000000;
    std::int64_t frequency_step_hz = 1;
    std::int32_t gain_step_tenths_db = 1;
    std::int64_t sample_rate_hz = 480000;
    std::string active_channel_profile_id;
    ReceiveAgcConfig receive_agc;
    ReceiveGainControlConfig receive_gain_control;
    RxFrontendConditioningConfig rx_frontend_conditioning;
    RxSignalCalibrationConfig rx_signal_calibration;
};

struct LocalPttTxConfig {
    int slot = 1;
    int color_code = 1;
    CallType call_type = CallType::AllCall;
    std::uint32_t destination_id = 0xFFFFFFU;
};

struct DmrConfig {
    DmrProfile profile = DmrProfile::T2;
    std::int32_t receive_squelch_tenths_dbfs = -740;
    int receive_inactivity_timeout_ms = 500;
    std::uint32_t repeater_id = 0;
    LocalPttTxConfig local_ptt_tx;
};

struct RoutingConfig {
    std::string policy = "route_all_valid_etsi";
    std::vector<std::uint32_t> source_id_whitelist;
    std::vector<std::uint32_t> destination_id_whitelist;
};

struct DataConfig {
    bool enabled = false;
};

struct TransmitConfig {
    bool enabled = true;
    std::string authorization_mode = "preauthorized";
    bool require_pretransmit_confirmation = false;
    int maximum_continuous_seconds = 600;
    int source_cooldown_seconds = 30;
    int hangtime_ms = 500;
};

struct LocalAudioConfig {
    bool monitor_enabled = true;
    bool input_enabled = true;
    std::string backend = "alsa";
    std::string capture_device;
    std::string playback_device;
    int sample_rate_hz = 8000;
    std::string sample_format = "s16le";
    int channels = 1;
    bool echo_cancellation = false;
    std::string recording_format = "mp3";
    std::filesystem::path recording_directory;
};

struct RemoteGatewayConfig {
    bool enabled = false;
    std::string implementation = "reserved";
    bool reject_enable = true;
};

struct UdpControlConfig {
    bool enabled = true;
    std::string protocol = "dmr-rpt-udp/1";
    std::string bind_address = "127.0.0.1";
    int port = 42000;
    int maximum_datagram_bytes = 1200;
    int default_lease_ms = 1000;
    int maximum_lease_ms = 1000;
    std::string control_token;
};

struct TcpStatusConfig {
    bool enabled = true;
    std::string protocol = "dmr-rpt-tcp/1";
    std::string bind_address = "127.0.0.1";
    int port = 42001;
    int maximum_clients = 8;
    int interval_ms = 1000;
};

struct RemoteVoiceConfig {
    bool enabled = false;
    std::string protocol = "dmr-rpt-ambe/1";
    std::string server_address = "127.0.0.1";
    int server_port = 42100;
    std::string device_id = "repeater-1";
    std::filesystem::path spool_directory;
    int connect_timeout_ms = 3000;
    int upload_timeout_ms = 10000;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    std::string feature = "DMR-RPT-AMBE-RECORDING-V1-000001";
};

struct LoggingConfig {
    std::filesystem::path event_directory;
    std::filesystem::path recording_directory;
    std::string startup_file_prefix = "events";
    std::string retention_policy = "retain_forever";
    bool automatic_cleanup = false;
    bool rotation_enabled = false;
    bool remote_archive_enabled = false;
    int max_queue_events = 4096;
};

struct RepeaterConfig {
    int version = 0;
    RadioConfig radio;
    IoStatusConfig io_status;
    std::vector<ChannelProfile> channel_profiles;
    DmrConfig dmr;
    RoutingConfig routing;
    DataConfig data;
    TransmitConfig transmit;
    LocalAudioConfig local_audio;
    RemoteGatewayConfig remote_gateway;
    UdpControlConfig udp_control;
    TcpStatusConfig tcp_status;
    RemoteVoiceConfig remote_voice;
    LoggingConfig logging;
    std::map<std::string, std::string> contract_versions;
};

struct ValidatedRfConfig {
    std::string active_channel_profile_id;
    std::size_t configured_channel_profile_count = 0;
    ChannelProfile active_profile;
    RadioConfig radio;
    IoStatusConfig io_status;
};

struct ValidatedConfig {
    RepeaterConfig config;
    ValidatedRfConfig rf;
    std::string semantic_sha256;
};

RepeaterConfig load_config_file(const std::filesystem::path& path);
RepeaterConfig load_config_string(const std::string& yaml_text);
void persist_active_channel_profile(const std::filesystem::path& path,
                                    const std::string& profile_id);
void persist_channel_profile(const std::filesystem::path& path,
                             const ChannelProfile& profile);
void persist_rx_signal_calibration(
    const std::filesystem::path& path,
    const RxSignalCalibrationConfig& calibration);
bool is_selectable_receive_gain_mode(const std::string& mode);
bool is_receive_gain_selection_mode(const std::string& mode);
std::int32_t receive_gain_tenths_db_for_mode(
    const ReceiveGainControlConfig& config, const std::string& mode);
ValidatedConfig validate_config(const RepeaterConfig& config);
std::string canonical_config_summary(const RepeaterConfig& config);

ChannelProfile make_lab_20260804_loopback_profile();
ChannelProfile make_lab_vhf_to_uhf_profile();

} // namespace dmr_rpt
