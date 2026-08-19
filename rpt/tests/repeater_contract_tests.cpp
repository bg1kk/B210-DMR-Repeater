// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/analog_fm.h"
#include "dmr_rpt/audit.h"
#include "dmr_rpt/config.h"
#include "dmr_rpt/dmr_burst.h"
#include "dmr_rpt/interlock.h"
#include "dmr_rpt/io_status.h"
#include "dmr_rpt/rf_session.h"
#include "dmr_rpt/recording.h"
#include "dmr_rpt/remote_voice.h"
#include "dmr_rpt/receive_agc.h"
#include "dmr_rpt/receive_signal_metrics.h"
#include "dmr_rpt/router.h"
#include "dmr_rpt/vector_manifest.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_config_error(Fn&& fn, const std::string& message)
{
    try {
        fn();
    } catch (const dmr_rpt::ConfigError&) {
        return;
    }
    throw std::runtime_error("expected ConfigError: " + message);
}

std::filesystem::path unique_temp_dir()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("dmr_rpt_tests_" + std::to_string(ticks));
    std::filesystem::create_directories(dir);
    return dir;
}

class FakeGpio final : public dmr_rpt::B210GpioAdapter {
public:
    dmr_rpt::GpioCapability capability(const std::string& bank) override
    {
        if (bank != "FP0") {
            return {bank, {}, {}};
        }
        return {"FP0", {0, 1, 2, 3, 4, 5}, {0, 1, 2, 3, 4, 5}};
    }

    bool configure_output(const std::string&, int pin) override
    {
        configured.push_back(pin);
        return !fail_configure;
    }

    bool write(const std::string&, int pin, dmr_rpt::IoLevel level) override
    {
        writes.push_back({pin, level});
        if (fail_write) {
            return false;
        }
        levels[pin] = level;
        return true;
    }

    bool configure_input(const std::string&, int pin) override
    {
        configured_inputs.push_back(pin);
        return true;
    }

    std::optional<std::uint32_t> read_mask(
        const std::string&, std::uint32_t mask) override
    {
        read_masks.push_back(mask);
        if (fail_read) return std::nullopt;
        return input_value & mask;
    }

    bool fail_configure = false;
    bool fail_write = false;
    bool fail_read = false;
    std::uint32_t input_value = 0;
    std::vector<int> configured;
    std::vector<int> configured_inputs;
    std::vector<std::pair<int, dmr_rpt::IoLevel>> writes;
    std::vector<std::uint32_t> read_masks;
    std::map<int, dmr_rpt::IoLevel> levels;
};

class SelectiveSession final : public dmr_rpt::B210Session {
public:
    explicit SelectiveSession(int& live_sessions, bool& fail_health)
        : live_sessions_(live_sessions), fail_health_(fail_health) {}

    ~SelectiveSession() override
    {
        --live_sessions_;
    }

    void start(const dmr_rpt::ValidatedRfConfig& config) override
    {
        if (config.active_channel_profile_id == "bad") {
            throw std::runtime_error("candidate rejected by fake RF");
        }
        started = true;
    }

    void stop() override
    {
        started = false;
    }

    bool health_check(std::string& error) const override
    {
        if (fail_health_) {
            error = "injected B210 health failure";
            return false;
        }
        error.clear();
        return true;
    }

    bool started = false;

private:
    int& live_sessions_;
    bool& fail_health_;
};

class SelectiveFactory final : public dmr_rpt::B210SessionFactory {
public:
    std::unique_ptr<dmr_rpt::B210Session> create() override
    {
        if (live_sessions != 0) created_while_previous_session_alive = true;
        ++count;
        ++live_sessions;
        return std::make_unique<SelectiveSession>(live_sessions, fail_health);
    }

    int count = 0;
    int live_sessions = 0;
    bool created_while_previous_session_alive = false;
    bool fail_health = false;
};

void test_config(const std::filesystem::path& config_path)
{
    dmr_rpt::RepeaterConfig config = dmr_rpt::load_config_file(config_path);
    dmr_rpt::ValidatedConfig validated = dmr_rpt::validate_config(config);
    require(validated.rf.configured_channel_profile_count == 8,
            "example config must have 8 profiles");
    require(validated.rf.active_profile.dmr_rx.frequency_hz == 145400000,
            "active VHF RX frequency");
    require(validated.rf.active_profile.dmr_tx.frequency_hz == 438500000,
            "active UHF TX frequency");
    require(validated.rf.active_profile.dmr_rx.frequency_hz !=
                validated.rf.active_profile.dmr_tx.frequency_hz,
            "RX/TX frequencies stay independent");
    require(validated.config.routing.source_id_whitelist.empty(),
            "source whitelist must be empty");
    require(validated.config.dmr.receive_squelch_tenths_dbfs == -740,
            "default DMR receive squelch is -74.0 dBFS");
    require(validated.config.radio.receive_agc.enabled &&
                validated.config.radio.receive_agc.target_tenths_dbfs == -200 &&
                validated.config.radio.receive_agc.minimum_gain_tenths_db == -300 &&
                validated.config.radio.receive_agc.maximum_gain_tenths_db == 300 &&
                validated.config.radio.receive_agc.attack_tenths_db_per_second == 6000 &&
                validated.config.radio.receive_agc.release_tenths_db_per_second == 1200,
            "receive AGC default profile is configured");
    require(validated.config.radio.receive_gain_control.high_gain_tenths_db == 500 &&
                validated.config.radio.receive_gain_control.medium_gain_tenths_db == 250 &&
                validated.config.radio.receive_gain_control.low_gain_tenths_db == 0 &&
                validated.config.radio.receive_gain_control.default_mode == "auto" &&
                validated.config.radio.receive_gain_control.automatic_switching.enabled &&
                validated.config.radio.receive_gain_control.automatic_switching
                    .high_to_low_threshold_dbm == -70 &&
                validated.config.radio.receive_gain_control.automatic_switching
                    .low_to_high_threshold_dbm == -60,
            "receive gain control defaults are configured");
    require(!validated.config.radio.rx_frontend_conditioning.enabled &&
                validated.config.radio.rx_frontend_conditioning.stage_bit0_io == 4 &&
                validated.config.radio.rx_frontend_conditioning.stage_bit1_io == 5 &&
                validated.config.radio.rx_frontend_conditioning.low_attenuation_db[0] == 0.0,
            "frontend conditioning defaults reserve IO4/IO5 safely");
    require(!validated.config.data.enabled,
            "DMR data decode, relay, and active send remain disabled");
    require(validated.config.contract_versions.at("RF") == "1.1.1" &&
                validated.config.contract_versions.at("AIR") == "0.6.2" &&
                validated.config.contract_versions.at("RPT") == "0.7.0" &&
                validated.config.contract_versions.at("NET") == "1.1.3" &&
                validated.config.contract_versions.at("UDP") == "0.12.3" &&
                validated.config.contract_versions.at("CAL") == "2.0.1" &&
                validated.config.contract_versions.at("SAFE") == "0.4.5" &&
                validated.config.contract_versions.at("AFM") == "0.2.5" &&
                validated.config.contract_versions.at("LOG") == "0.3.2" &&
                validated.config.contract_versions.at("IO") == "0.3.0",
            "runtime contract versions are current");
    require(validated.config.tcp_status.protocol == "dmr-rpt-tcp/1" &&
                validated.config.tcp_status.interval_ms == 1000,
            "TCP status publishes on the one-second contract interval");
    require(validated.config.remote_voice.protocol == "dmr-rpt-ambe/1" &&
                validated.config.remote_voice.feature.size() == 32U,
            "remote AMBE recording uses a fixed 32-byte feature string");
    for (const auto& profile : validated.config.channel_profiles) {
        require(profile.analog_fm_fallback.dmr_tx.source_id == 9999 &&
                    profile.analog_fm_fallback.dmr_tx.destination_id == 0xFFFFFFU &&
                    profile.analog_fm_fallback.dmr_tx.call_type ==
                        dmr_rpt::CallType::AllCall,
                "analog FM DMR identity is fixed to source 9999 all-call");
        if (profile.analog_fm_fallback.enabled) {
            require(profile.analog_fm_fallback.ctcss.required &&
                        profile.analog_fm_fallback.ctcss.tone_tenths_hz > 0,
                    "enabled analog FM requires a configured CTCSS tone");
            require(dmr_rpt::analog_fm_relay_start_latency_bound_ms(
                        profile.analog_fm_fallback) <= 500,
                    "enabled analog FM has a relay start bound of 500 ms");
        }
    }
    require(validated.config.channel_profiles[0]
                    .analog_fm_fallback.ctcss.tone_tenths_hz == 1230,
            "omitted per-channel CTCSS tone defaults to 123.0 Hz");
    require(validated.config.channel_profiles[1]
                    .analog_fm_fallback.ctcss.tone_tenths_hz == 885,
            "each channel profile may configure an independent CTCSS tone");
    require(!validated.semantic_sha256.empty(), "semantic SHA-256 must exist");

    const std::filesystem::path persistence_dir = unique_temp_dir();
    const std::filesystem::path persistence_config =
        persistence_dir / "repeater.yaml";
    std::filesystem::copy_file(config_path, persistence_config,
                               std::filesystem::copy_options::overwrite_existing);
    const std::string persisted_profile_id = config.channel_profiles.at(1).id;
    dmr_rpt::persist_active_channel_profile(persistence_config,
                                            persisted_profile_id);
    const dmr_rpt::RepeaterConfig persisted_config =
        dmr_rpt::load_config_file(persistence_config);
    require(persisted_config.radio.active_channel_profile_id ==
                persisted_profile_id,
            "persisted active channel profile is reloaded");
    require(dmr_rpt::validate_config(persisted_config)
                .rf.active_channel_profile_id == persisted_profile_id,
            "persisted active channel profile validates");
    dmr_rpt::ChannelProfile saved_profile = persisted_config.channel_profiles.at(1);
    saved_profile.dmr_rx.frequency_hz += 12500;
    saved_profile.dmr_tx.frequency_hz += 12500;
    saved_profile.dmr_rx.gain_tenths_db += 10;
    saved_profile.analog_fm_fallback.ctcss.tone_tenths_hz = 1000;
    dmr_rpt::persist_channel_profile(persistence_config, saved_profile);
    const dmr_rpt::RepeaterConfig saved_config =
        dmr_rpt::load_config_file(persistence_config);
    const dmr_rpt::ChannelProfile& reloaded_profile =
        saved_config.channel_profiles.at(1);
    require(reloaded_profile.dmr_rx.frequency_hz == saved_profile.dmr_rx.frequency_hz &&
                reloaded_profile.dmr_tx.frequency_hz == saved_profile.dmr_tx.frequency_hz &&
                reloaded_profile.dmr_rx.gain_tenths_db == saved_profile.dmr_rx.gain_tenths_db &&
                reloaded_profile.analog_fm_fallback.ctcss.tone_tenths_hz ==
                    saved_profile.analog_fm_fallback.ctcss.tone_tenths_hz,
            "saved channel profile is reloaded without RF activation");
    std::error_code persistence_cleanup_error;
    std::filesystem::remove_all(persistence_dir, persistence_cleanup_error);

    dmr_rpt::RepeaterConfig bad = config;
    bad.channel_profiles.resize(7);
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "fewer than 8 profiles");

    bad = config;
    bad.channel_profiles[1].id = bad.channel_profiles[0].id;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "duplicate profile id");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.dmr_tx.source_id = 1001;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM source ID override");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.dmr_tx.destination_id = 2001;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM destination ID override");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.dmr_tx.call_type =
        dmr_rpt::CallType::Group;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM call type override");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.enabled = true;
    bad.channel_profiles[0].analog_fm_fallback.ctcss.tone_tenths_hz = 0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM non-positive CTCSS");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.enabled = true;
    bad.channel_profiles[0].analog_fm_fallback.ctcss.required = false;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM without required CTCSS");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.enabled = true;
    bad.channel_profiles[0].analog_fm_fallback.fm.max_deviation_hz = 0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM non-positive deviation");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.enabled = true;
    bad.channel_profiles[0].analog_fm_fallback.fm.squelch_tenths_dbfs = 0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM non-negative squelch");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.enabled = true;
    bad.channel_profiles[0].analog_fm_fallback.dmr_idle_guard_ms = 100;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog FM relay latency over 500 ms");

    bad = config;
    bad.radio.active_channel_profile_id = "missing";
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "missing active profile");

    dmr_rpt::RepeaterConfig uhd_override = config;
    uhd_override.radio.uhd_device = "type=b200,serial=12345678";
    require(dmr_rpt::validate_config(uhd_override).rf.radio.uhd_device ==
                "type=b200,serial=12345678",
            "B210 UHD device parameters accept an explicit serial");

    bad = config;
    bad.radio.uhd_device = "type=x300";
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "non-B210 UHD device parameters");

    bad = config;
    bad.channel_profiles[0].analog_fm_fallback.rx.channel =
        bad.channel_profiles[0].dmr_rx.channel;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "analog RX channel conflict");

    bad = config;
    bad.io_status.pins[1].io = bad.io_status.pins[0].io;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "duplicate IO pin");

    bad = config;
    bad.remote_gateway.enabled = true;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "remote gateway must remain reserved");

    bad = config;
    bad.routing.source_id_whitelist.push_back(1001);
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "runtime whitelist forbidden");

    bad = config;
    bad.dmr.receive_squelch_tenths_dbfs = 0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "DMR receive squelch must be below 0 dBFS");

    bad = config;
    bad.radio.receive_agc.minimum_gain_tenths_db = 10;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "receive AGC must permit strong-signal attenuation");

    bad = config;
    bad.radio.receive_agc.maximum_gain_tenths_db = -10;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "receive AGC must permit weak-signal gain");

    bad = config;
    bad.radio.receive_gain_control.high_gain_tenths_db =
        bad.radio.receive_gain_control.medium_gain_tenths_db;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "receive gain control high/medium order");

    bad = config;
    bad.radio.receive_gain_control.low_gain_tenths_db = 10;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "automatic receive low gain is fixed at zero");

    require(dmr_rpt::receive_gain_tenths_db_for_mode(
                config.radio.receive_gain_control, "high") == 500 &&
                dmr_rpt::receive_gain_tenths_db_for_mode(
                    config.radio.receive_gain_control, "medium") == 250 &&
                dmr_rpt::receive_gain_tenths_db_for_mode(
                    config.radio.receive_gain_control, "low") == 0,
            "receive gain modes resolve configured gains");
    require(dmr_rpt::is_receive_gain_selection_mode("auto") &&
                dmr_rpt::is_receive_gain_selection_mode("high") &&
                dmr_rpt::is_receive_gain_selection_mode("medium") &&
                !dmr_rpt::is_receive_gain_selection_mode("custom"),
            "receive gain selection permits auto and three fixed ranges");

    bad = config;
    bad.radio.rx_frontend_conditioning.low_attenuation_db[0] = 1.0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "frontend stage zero is fixed at zero dB");

    bad = config;
    bad.radio.rx_frontend_conditioning.high_attenuation_db[2] =
        bad.radio.rx_frontend_conditioning.high_attenuation_db[1];
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "frontend attenuation values are strictly increasing");

    bad = config;
    bad.radio.receive_gain_control.automatic_switching
        .high_to_low_threshold_dbm = -69;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "automatic receive gain thresholds are fixed");

    bad = config;
    bad.radio.receive_agc.attack_tenths_db_per_second = 0;
    require_config_error([&]() { dmr_rpt::validate_config(bad); },
                         "receive AGC attack rate must be positive");

    const dmr_rpt::ChannelProfile lab = dmr_rpt::make_lab_20260804_loopback_profile();
    require(lab.dmr_rx.gain_tenths_db == 500 && lab.dmr_tx.gain_tenths_db == 850,
            "2026-08-04 lab fixture gains");
}

void test_router_and_safe(const dmr_rpt::RepeaterConfig& config)
{
    const std::int64_t now = 100000;
    dmr_rpt::DmrSourceCooldownStore cooldowns;
    dmr_rpt::CallRouter router(config.routing, config.transmit,
                               [&](std::uint32_t source_id) {
                                   return cooldowns.check(source_id, now);
                               });

    dmr_rpt::DmrEvent private_call;
    private_call.profile = config.dmr.profile;
    private_call.kind = dmr_rpt::DmrEventKind::Voice;
    private_call.integrity = dmr_rpt::DmrIntegrity::Valid;
    private_call.source_id = 100103;
    private_call.destination_id = 100102;
    private_call.call_type = dmr_rpt::CallType::Private;
    dmr_rpt::RouteDecision decision = router.route_dmr_event(private_call);
    require(decision.accepted, "private call accepted");

    dmr_rpt::DmrEvent group_call = private_call;
    group_call.destination_id = 2001;
    group_call.call_type = dmr_rpt::CallType::Group;
    require(router.route_dmr_event(group_call).accepted, "group call accepted");

    dmr_rpt::DmrEvent all_call = private_call;
    all_call.destination_id = 16777215;
    all_call.call_type = dmr_rpt::CallType::AllCall;
    require(router.route_dmr_event(all_call).accepted, "all call accepted");

    dmr_rpt::DmrEvent raw_data;
    raw_data.profile = config.dmr.profile;
    raw_data.kind = dmr_rpt::DmrEventKind::RawData;
    raw_data.integrity = dmr_rpt::DmrIntegrity::Valid;
    require(!router.route_dmr_event(raw_data).accepted,
            "DMR data is rejected while the data service is disabled");

    dmr_rpt::DmrEvent invalid = private_call;
    invalid.integrity = dmr_rpt::DmrIntegrity::Invalid;
    require(!router.route_dmr_event(invalid).accepted, "invalid integrity rejected");

    cooldowns.record_duration_limit(100103, now, config.transmit.source_cooldown_seconds);
    require(router.route_dmr_event(private_call).reason ==
                dmr_rpt::RouteRejectReason::SourceCooldown,
            "same source cooldown");
    require(router.route_dmr_event(group_call).reason ==
                dmr_rpt::RouteRejectReason::SourceCooldown,
            "cooldown crosses call type");
    dmr_rpt::DmrEvent other_source = private_call;
    other_source.source_id = 42;
    require(router.route_dmr_event(other_source).accepted,
            "different source is not globally cooled down");

    dmr_rpt::AutomaticTransmitGate gate(config.transmit);
    dmr_rpt::AutomaticTransmitGrant grant =
        gate.evaluate(*decision.tx_request, {}, now);
    require(grant.granted, "default preauthorized grant");
    gate.set_transmit_enabled(false);
    require(!gate.evaluate(*decision.tx_request, {}, now).granted,
            "transmit disabled rejects grant");
    gate.set_transmit_enabled(true);
    gate.set_rebuild_inhibit(true);
    require(gate.evaluate(*decision.tx_request, {}, now).reason ==
                dmr_rpt::RouteRejectReason::ConfigurationRebuildInhibit,
            "rebuild inhibit rejects grant");

    dmr_rpt::SourceCooldownInfo before =
        cooldowns.check(100103, now + 29999);
    dmr_rpt::SourceCooldownInfo at_deadline =
        cooldowns.check(100103, now + 30000);
    require(before.active, "cooldown active before 30 seconds");
    require(!at_deadline.active, "cooldown clears at 30 seconds");

    dmr_rpt::TxRequest analog;
    analog.origin = dmr_rpt::TxOrigin::AnalogFm;
    analog.profile = config.dmr.profile;
    analog.source_id = 9999;
    analog.destination_id = 0xFFFFFFU;
    analog.call_type = dmr_rpt::CallType::AllCall;
    analog.slot = 1;
    analog.color_code = 1;
    require(router.route_analog_fm(analog, true, true).accepted,
            "qualified idle analog FM fixed all-call is accepted");
    require(router.route_analog_fm(analog, false, true).reason ==
                dmr_rpt::RouteRejectReason::AnalogFmSuppressed,
            "analog FM without CTCSS is suppressed");
    require(router.route_analog_fm(analog, true, false).reason ==
                dmr_rpt::RouteRejectReason::AnalogFmSuppressed,
            "analog FM while DMR is active is suppressed");
    analog.source_id = 1001;
    require(router.route_analog_fm(analog, true, true).reason ==
                dmr_rpt::RouteRejectReason::InvalidRequest,
            "analog FM source identity override is rejected by the router");
}

void test_receive_agc()
{
    dmr_rpt::ReceiveAgcConfig config;
    dmr_rpt::ReceiveAgcController agc(config);
    const double weak_power = std::pow(10.0, -50.0 / 10.0);
    for (int index = 0; index < 25; ++index) {
        agc.observe_average_power(weak_power, 0.01);
    }
    dmr_rpt::ReceiveAgcSnapshot snapshot = agc.snapshot();
    require(snapshot.input_dbfs && snapshot.output_dbfs &&
                std::abs(*snapshot.input_dbfs + 50.0) < 0.01 &&
                std::abs(snapshot.gain_db - 30.0) < 0.01 &&
                std::abs(*snapshot.output_dbfs + 20.0) < 0.01,
            "receive AGC raises a weak admitted signal to the configured target");

    const double strong_power = std::pow(10.0, -5.0 / 10.0);
    for (int index = 0; index < 8; ++index) {
        agc.observe_average_power(strong_power, 0.01);
    }
    snapshot = agc.snapshot();
    require(snapshot.input_dbfs && snapshot.output_dbfs &&
                std::abs(snapshot.gain_db + 15.0) < 0.01 &&
                std::abs(*snapshot.output_dbfs + 20.0) < 0.01,
            "receive AGC attenuates a strong admitted signal at the attack rate");

    agc.observe_average_power(std::pow(10.0, -85.0 / 10.0), 0.01, -74.0);
    snapshot = agc.snapshot();
    require(snapshot.input_dbfs && std::abs(*snapshot.input_dbfs + 85.0) < 0.01 &&
                std::abs(snapshot.gain_db) < 0.01,
            "receive AGC restores unity below the squelch threshold");

    agc.reset();
    snapshot = agc.snapshot();
    require(!snapshot.input_dbfs && std::abs(snapshot.gain_db) < 0.01 &&
                std::abs(agc.gain_linear() - 1.0) < 0.01,
            "receive AGC reset restores unity gain");

    const double held_gain = snapshot.gain_db;
    agc.observe_average_power(0.0, 1.0);
    require(std::abs(agc.snapshot().gain_db - held_gain) < 0.01,
            "receive AGC does not chase post-squelch zero samples");

    config.enabled = false;
    dmr_rpt::ReceiveAgcController disabled(config);
    disabled.observe_average_power(weak_power, 1.0);
    require(std::abs(disabled.snapshot().gain_db) < 0.01 &&
                std::abs(disabled.gain_linear() - 1.0) < 0.01,
            "disabled receive AGC remains unity gain");
}

void test_rx_signal_calibration()
{
    const auto& low = dmr_rpt::rx_calibration_required_inputs(
        dmr_rpt::RxCalibrationBand::Low);
    const auto& medium = dmr_rpt::rx_calibration_required_inputs(
        dmr_rpt::RxCalibrationBand::Medium);
    const auto& high = dmr_rpt::rx_calibration_required_inputs(
        dmr_rpt::RxCalibrationBand::High);
    require(low == std::vector<int>({-55, -50, -45, -40, -35, -30, -25, -20}),
            "low calibration input ladder is -20 through -55 dBm");
    require(medium == std::vector<int>({-85, -80, -75, -70, -65, -60, -55,
                                        -50, -45}),
            "medium calibration input ladder is -45 through -85 dBm");
    require(high == std::vector<int>({-125, -120, -115, -110, -105, -100, -95,
                                      -90, -85, -80, -75}),
            "high calibration input ladder is -75 through -125 dBm");

    dmr_rpt::RxSignalCalibrationCurve low_curve;
    low_curve.rx_gain_tenths_db = 0;
    for (const int input_dbm : low) {
        low_curve.points.push_back(
            {input_dbm, static_cast<double>(input_dbm) - 20.0, 10.0, {}});
    }
    require(dmr_rpt::rx_calibration_curve_complete(
                low_curve, dmr_rpt::RxCalibrationBand::Low),
            "complete low calibration requires fixed zero RX gain");

    dmr_rpt::RxSignalCalibrationCurve invalid_low = low_curve;
    invalid_low.rx_gain_tenths_db = 10;
    require(!dmr_rpt::rx_calibration_curve_complete(
                invalid_low, dmr_rpt::RxCalibrationBand::Low),
            "low calibration rejects non-zero RX gain");

    dmr_rpt::RxSignalCalibrationCurve medium_curve;
    medium_curve.rx_gain_tenths_db = 250;
    for (const int input_dbm : medium) {
        medium_curve.points.push_back(
            {input_dbm, static_cast<double>(input_dbm) + 5.0, 10.0, {}});
    }
    require(dmr_rpt::rx_calibration_curve_complete(
                medium_curve, dmr_rpt::RxCalibrationBand::Medium),
            "complete medium calibration accepts a locked positive RX gain");

    dmr_rpt::RxSignalCalibrationCurve high_curve;
    high_curve.rx_gain_tenths_db = 500;
    for (const int input_dbm : high) {
        high_curve.points.push_back(
            {input_dbm, static_cast<double>(input_dbm) + 30.0, 12.0, {}});
    }
    require(dmr_rpt::rx_calibration_curve_complete(
                high_curve, dmr_rpt::RxCalibrationBand::High),
            "complete high calibration accepts a locked positive RX gain");

    dmr_rpt::RxSignalCalibrationConfig calibration;
    calibration.low[0] = low_curve;
    calibration.medium[0] = medium_curve;
    calibration.high[0] = high_curve;
    const dmr_rpt::RxCalibrationReading low_reading =
        dmr_rpt::rx_calibration_reading(calibration, 0, 0, -65.0);
    require(low_reading.calibrated && low_reading.rssi_dbm &&
                std::abs(*low_reading.rssi_dbm + 45.0) < 0.001,
            "low calibration interpolates measured dBFS into dBm");
    const dmr_rpt::RxCalibrationReading compensated =
        dmr_rpt::rx_calibration_reading(calibration, 0, 450, -65.0);
    require(compensated.calibrated && compensated.rssi_dbm &&
                std::abs(*compensated.rssi_dbm + 90.0) < 0.001 &&
                compensated.reference_gain_tenths_db ==
                    std::optional<std::int32_t>(500) &&
                compensated.gain_compensation_db ==
                    std::optional<double>(5.0) &&
                compensated.compensated_dbfs ==
                    std::optional<double>(-60.0),
            "hardware AGC gain is translated to the nearest calibration gain");
    require(dmr_rpt::rx_calibration_reference_dbfs(
                calibration, 0, dmr_rpt::RxCalibrationBand::High, -90, 500) ==
                std::optional<double>(-60.0) &&
                dmr_rpt::rx_calibration_reference_dbfs(
                    calibration, 0, dmr_rpt::RxCalibrationBand::Low, -50, 0) ==
                    std::optional<double>(-70.0) &&
                !dmr_rpt::rx_calibration_reference_dbfs(
                    calibration, 0, dmr_rpt::RxCalibrationBand::High, -90, 0),
            "calibration exposes only matching-gain automatic-switch anchors");

    dmr_rpt::RxSignalCalibrationCurve weak_high = high_curve;
    weak_high.points.front().snr_db = 11.9;
    require(!dmr_rpt::rx_calibration_curve_complete(
                weak_high, dmr_rpt::RxCalibrationBand::High),
            "high calibration rejects SNR below 12 dB");

    dmr_rpt::RxSignalCalibrationRuntime runtime(calibration);
    for (int index = 0; index < 5; ++index) {
        runtime.observe(0, 0, -45.0 + index * 0.1, -70.0, 5.0,
                        1000 + index * 200);
    }
    require(runtime.stable_observation(0, 5U, 1800, 1000, 1.0).has_value(),
            "five fresh samples within 1 dB are stable");
    runtime.clear_observations(0);
    require(!runtime.stable_observation(0, 5U, 1800, 1000, 1.0).has_value(),
            "clearing calibration observations removes stale stability evidence");
}


std::vector<float> make_tone(double frequency_hz,
                             int duration_ms,
                             double amplitude = 0.2,
                             int sample_rate_hz = 8000)
{
    constexpr double pi = 3.14159265358979323846;
    const std::size_t count = static_cast<std::size_t>(
        static_cast<std::int64_t>(duration_ms) * sample_rate_hz / 1000);
    std::vector<float> samples(count);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<float>(amplitude * std::sin(
            2.0 * pi * frequency_hz * static_cast<double>(index) /
            static_cast<double>(sample_rate_hz)));
    }
    return samples;
}

std::vector<float> make_dmr_like_discriminator(int duration_ms,
                                               int sample_rate_hz = 8000)
{
    const std::size_t count = static_cast<std::size_t>(
        static_cast<std::int64_t>(duration_ms) * sample_rate_hz / 1000);
    std::vector<float> samples(count);
    std::uint32_t lfsr = 0x5A17U;
    std::int64_t symbol_phase = 0;
    float level = 0.0F;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        if (symbol_phase <= 0) {
            const unsigned dibit = lfsr & 0x03U;
            level = std::array<float, 4>{-0.30F, -0.10F, 0.10F, 0.30F}[dibit];
            const std::uint32_t feedback =
                ((lfsr >> 0U) ^ (lfsr >> 2U) ^
                 (lfsr >> 3U) ^ (lfsr >> 5U)) & 1U;
            lfsr = (lfsr >> 1U) | (feedback << 15U);
            symbol_phase += sample_rate_hz;
        }
        samples[index] = level;
        symbol_phase -= 4800;
    }
    return samples;
}

void test_ctcss_detector()
{
    dmr_rpt::CtcssConfig config{true, 1230, 120, 250, 200};
    dmr_rpt::CtcssDetector short_detector(config);
    require(short_detector.process(make_tone(123.0, 249)).qualified == false,
            "CTCSS shorter than minimum detect time is rejected");
    dmr_rpt::CtcssDetector detector(config);
    require(detector.process(make_tone(123.0, 310)).qualified,
            "configured 123 Hz CTCSS qualifies after phase verification");
    require(detector.state().confidence_db >= 12.0,
            "configured CTCSS reaches the confidence threshold");

    require(detector.process(make_tone(110.9, 180)).qualified,
            "CTCSS release hold keeps the session qualified");
    require(!detector.process(make_tone(110.9, 40)).qualified,
            "wrong CTCSS releases after the configured hold time");

    dmr_rpt::CtcssDetector wrong(config);
    require(!wrong.process(make_tone(118.8, 600)).qualified,
            "adjacent non-target CTCSS is rejected");

    dmr_rpt::CtcssDetector dmr_like(config);
    require(!dmr_like.process(make_dmr_like_discriminator(1200)).qualified,
            "DMR-like four-level discriminator data is not classified as FM");

    config.tone_tenths_hz = 885;
    dmr_rpt::CtcssDetector independent(config);
    require(independent.process(make_tone(88.5, 300)).qualified,
            "per-profile 88.5 Hz CTCSS qualifies independently");
    independent.reset();
    require(!independent.process(make_tone(123.0, 500)).qualified,
            "a detector configured for 88.5 Hz rejects 123 Hz");

    require(dmr_rpt::kAnalogFmMaxQueuedAmbeFrames * 20U <= 180U,
            "FM audio queue contributes at most 180 ms of backlog");

    require(dmr_rpt::analog_fm_can_qualify(true, false, false),
            "released DMR lock permits qualified FM");
    require(!dmr_rpt::analog_fm_can_qualify(true, true, false),
            "DMR rearm lock suppresses a still-qualified CTCSS detector");
    require(!dmr_rpt::analog_fm_can_qualify(true, false, true),
            "active DMR always suppresses FM");
}

void test_io(const dmr_rpt::IoStatusConfig& config)
{
    FakeGpio gpio;
    dmr_rpt::B210IoStatusController io(config, gpio);
    io.initialize(0);
    require(io.state().gpio_healthy, "GPIO initializes");
    require(gpio.configured.size() == 4, "four GPIO outputs configured");
    for (const auto& item : gpio.levels) {
        require(item.second == dmr_rpt::IoLevel::High, "initial level high");
    }

    io.on_rx_activity(1, true, true, 0);
    require(gpio.levels[1] == dmr_rpt::IoLevel::Low, "RX1 activity pulls IO1 low");
    io.on_rx_activity(1, false, false, 100);
    io.poll(599);
    require(gpio.levels[1] == dmr_rpt::IoLevel::Low, "RX release holds for 499 ms");
    io.poll(600);
    require(gpio.levels[1] == dmr_rpt::IoLevel::High, "RX release high at 500 ms");

    io.on_tx_ptt(0, true, 1000);
    require(gpio.levels[2] == dmr_rpt::IoLevel::Low, "TX0 PTT pulls IO2 low");
    io.on_tx_ptt(0, false, 1100);
    io.poll(1899);
    require(gpio.levels[2] == dmr_rpt::IoLevel::Low, "TX release holds for 799 ms");
    io.poll(1900);
    require(gpio.levels[2] == dmr_rpt::IoLevel::High, "TX release high at 800 ms");

    FakeGpio bad_gpio;
    bad_gpio.fail_write = true;
    dmr_rpt::B210IoStatusController bad(config, bad_gpio);
    bad.initialize(0);
    require(!bad.state().gpio_healthy, "GPIO write failure enters fault");

    dmr_rpt::RxFrontendConditioningConfig frontend_config;
    frontend_config.enabled = true;
    FakeGpio frontend_gpio;
    frontend_gpio.input_value = (std::uint32_t{1} << 4) |
        (std::uint32_t{1} << 5);
    dmr_rpt::B210FrontendStageController frontend(
        frontend_config, frontend_gpio);
    frontend.initialize("low");
    require(frontend.state().gpio_healthy && frontend.state().stage == 3 &&
                frontend.state().attenuation_db == 30.0,
            "frontend starts in the maximum attenuation stage");
    require(frontend_gpio.configured_inputs == std::vector<int>({4, 5}) &&
                frontend_gpio.read_masks.size() == 1U,
            "frontend stage uses two GPIO inputs read in one mask");
    frontend_gpio.input_value = std::uint32_t{1} << 5;
    require(frontend.poll("medium") &&
                frontend.state().gpio_code == 2 &&
                frontend.state().attenuation_db == 20.0,
            "frontend stage and range select the configured attenuation");
    frontend_gpio.input_value = 0;
    require(frontend.poll("medium"), "frontend stage zero read succeeds");
    require(frontend.state().stage == 0 &&
                frontend.state().attenuation_db == 0.0,
            "frontend input code zero uses fixed zero attenuation");
}

void test_audit(const dmr_rpt::LoggingConfig& config)
{
    dmr_rpt::LoggingConfig logging = config;
    logging.event_directory = unique_temp_dir();
    dmr_rpt::OperationAuditLogger audit(logging, []() {
        return std::string("2026-08-05T00:00:00Z");
    });
    audit.emit({"RPT", "dmr_relay.admitted", "forward_call", "accepted",
                "corr-1", {{"source_id", "1001"}, {"payload", "blocked"}}});
    audit.emit({"IO", "io_status.changed", "set_pin", "ok",
                "corr-2", {{"pin", "1"}, {"level", "low"}}});
    require(audit.health().written_events == 2, "audit wrote two events");

    std::ifstream input(audit.path());
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    require(content.find("\"event_seq\":1") != std::string::npos,
            "first sequence present");
    require(content.find("\"event_seq\":2") != std::string::npos,
            "second sequence present");
    std::istringstream lines(content);
    std::string line;
    int timestamped_lines = 0;
    while (std::getline(lines, line)) {
        require(line.find("\"Timestamp\":\"2026-08-05T00:00:00Z\"") !=
                    std::string::npos,
                "every JSONL record has the required Timestamp field");
        require(line.find("\"event_time_utc\":\"2026-08-05T00:00:00Z\"") !=
                    std::string::npos,
                "Timestamp and event_time_utc use the same UTC value");
        ++timestamped_lines;
    }
    require(timestamped_lines == 2, "both JSONL records were timestamp checked");
    require(content.find("payload") == std::string::npos,
            "blocked payload field absent");
}

void test_rf_reinit(const dmr_rpt::ValidatedRfConfig& rf)
{
    SelectiveFactory factory;
    dmr_rpt::RfReinitializationController controller(factory);
    require(controller.start_initial(rf).activated, "initial RF dry start");
    dmr_rpt::ValidatedRfConfig candidate = rf;
    candidate.active_channel_profile_id = "bad";
    dmr_rpt::RfReinitializationResult result =
        controller.reinitialize(candidate, rf);
    require(!result.activated, "bad candidate not activated");
    require(result.rolled_back, "bad candidate rolls back");
    require(!factory.created_while_previous_session_alive,
            "candidate session is created only after old RF session is destroyed");
    require(controller.running(), "controller running after rollback");
    controller.poll(1234);
    factory.fail_health = true;
    std::string health_error;
    require(!controller.health_check(health_error) &&
                health_error == "injected B210 health failure",
            "B210 health failure is reported to the RF controller");
    factory.fail_health = false;
    controller.stop();
    require(!controller.running(), "controller stops and releases current RF session");
    require(factory.live_sessions == 0,
            "stopped controller destroys the B210 session before restart");
    require(controller.start_initial(rf).activated,
            "stopped controller can create a fresh RF session on restart");
    require(controller.running(), "controller running after RF restart");
    controller.stop();
}

void test_manifest(const std::filesystem::path& vector_root)
{
    const std::filesystem::path voice_manifest =
        vector_root / "direct_voice_group_slot1_001" / "manifest.json";
    if (!std::filesystem::exists(voice_manifest)) {
        throw std::runtime_error("direct_lab manifest is missing");
    }
    dmr_rpt::ManifestVerification verification =
        dmr_rpt::verify_vector_manifest(voice_manifest, false);
    require(verification.ok, "direct_lab voice manifest hashes verify");
    require(verification.manifest.vector_id == "direct_voice_group_slot1_001",
            "voice vector id loaded");
    require(!dmr_rpt::t1_t2_data_vectors_available(vector_root),
            "T1/T2/DATA vectors intentionally gated");
}

void test_call_console_format()
{
    dmr_rpt::CallConsoleEvent started;
    started.label = "CALL START";
    started.source_id = 100103;
    started.destination_id = 2001;
    started.call_type = dmr_rpt::CallType::Group;
    started.color_code = 1;
    started.slot = 1;
    started.correlation_id = "rf-1";
    require(dmr_rpt::format_call_console_line(started) ==
                "Call Start Qrf-1 SID=100103>DID=2001 G C1T1",
            "call start console body follows the confirmed token registry");

    started.label = "RELAY START";
    require(dmr_rpt::format_call_console_line(started) ==
                "Rpt Start Qrf-1 SID=100103>DID=2001 G C1T1",
            "relay start console body follows the confirmed token registry");

    started.label = "CALL END";
    started.reason = "terminator";
    started.duration_ms = 10025;
    require(dmr_rpt::format_call_console_line(started) ==
                "Call End Qrf-1 SID=100103>DID=2001 G C1T1 RT D10s",
            "call end console body includes correlation, reason and duration");

    dmr_rpt::update_console_rx_power(0.01);
    const std::vector<std::string> console_lines =
        dmr_rpt::format_console_lines(
            dmr_rpt::format_call_console_bodies(started));
    require(console_lines.size() == 3U,
            "call event uses the registered continuation layout");
    for (const std::string& console_line : console_lines) {
        require(console_line.size() <= 40U,
                "every console continuation is limited to 40 bytes");
        require(console_line.find(" Rssi=-20.0 ") != std::string::npos,
                "console line includes the confirmed RX power token");
    }
    require(console_lines[0].find("Call End Qrf-1") != std::string::npos,
            "first call line includes event and correlation");
    require(console_lines[1].find("+SID=100103>DID=2001") !=
                std::string::npos,
            "second call line preserves both identities");
    require(console_lines[2].find("+G C1T1 RT D10s") != std::string::npos,
            "third call line preserves type, channel, reason and duration");

    require(!dmr_rpt::call_inactivity_expired(1000, 1499, 500),
            "call remains active before receive inactivity timeout");
    require(dmr_rpt::call_inactivity_expired(1000, 1500, 500),
            "call ends exactly at receive inactivity timeout");
    require(!dmr_rpt::call_inactivity_expired(1000, 999, 500),
            "backward clock observation does not expire call");
    require(dmr_rpt::format_duration_hms(-1) == "00:00:00",
            "negative duration is clamped to zero");
    require(dmr_rpt::format_duration_hms(999) == "00:00:00",
            "subsecond duration uses whole seconds");
    require(dmr_rpt::format_duration_hms(3661000) == "01:01:01",
            "relay duration is formatted as hours minutes seconds");

    started.reason = "inactivity_timeout";
    started.duration_ms = 10500;
    require(dmr_rpt::format_call_console_line(started) ==
                "Call End Qrf-1 SID=100103>DID=2001 G C1T1 RI D10s",
            "receive loss call end uses the inactivity abbreviation");

    require(std::string(dmr_rpt::console_token::Ctcss) == "CT" &&
                std::string(dmr_rpt::console_token::DisableFm) == "disFM" &&
                std::string(dmr_rpt::console_token::SquelchOpen) == "SQL_on" &&
                std::string(dmr_rpt::console_token::SquelchClosed) == "SQL_off" &&
                std::string(dmr_rpt::console_token::RecordingStart) ==
                    "Recorder Start" &&
                std::string(dmr_rpt::console_token::RecordingStop) ==
                    "Recorder Stop" &&
                std::string(dmr_rpt::console_token::RecordingFail) ==
                    "Recorder Fail",
            "confirmed startup, squelch and recording tokens are frozen");
}

void test_receive_signal_metrics()
{
    dmr_rpt::ReceiveSignalMetrics metrics(-74.0);
    metrics.observe_average_power(1e-9);
    dmr_rpt::ReceiveSignalSnapshot snapshot = metrics.snapshot();
    require(snapshot.signal_dbfs && snapshot.noise_dbfs && snapshot.snr_db,
            "noise observation produces a complete SNR snapshot");
    require(std::abs(*snapshot.signal_dbfs + 90.0) < 0.1 &&
                std::abs(*snapshot.noise_dbfs + 90.0) < 0.1 &&
                std::abs(*snapshot.snr_db) < 0.1,
            "idle signal reports the learned noise floor and zero SNR");

    metrics.observe_average_power(1e-6);
    snapshot = metrics.snapshot();
    require(snapshot.signal_dbfs && snapshot.noise_dbfs && snapshot.snr_db &&
                std::abs(*snapshot.signal_dbfs + 60.0) < 0.1 &&
                std::abs(*snapshot.noise_dbfs + 90.0) < 0.1 &&
                std::abs(*snapshot.snr_db - 30.0) < 0.1,
            "active signal reports SNR relative to the learned noise floor");

    dmr_rpt::ReceiveSignalMetrics dmr_metrics(-74.0, 200);
    dmr_metrics.observe_average_power(1e-9, 1000);
    dmr_metrics.observe_average_power(1e-6, 1100);
    dmr_metrics.observe_average_power(1e-9, 1130);
    snapshot = dmr_metrics.snapshot(1130);
    require(snapshot.signal_dbfs && snapshot.noise_dbfs && snapshot.snr_db &&
                std::abs(*snapshot.signal_dbfs + 60.0) < 0.1 &&
                *snapshot.snr_db > 20.0,
            "DMR off-slot gap retains active RSSI and SNR for 200 ms");

    snapshot = dmr_metrics.snapshot(1300);
    require(snapshot.signal_dbfs && snapshot.snr_db &&
                std::abs(*snapshot.signal_dbfs + 90.0) < 0.1 &&
                std::abs(*snapshot.snr_db) < 0.1,
            "DMR RSSI and SNR return to idle after the release hold");

    snapshot = metrics.snapshot();
    require(snapshot.signal_dbfs && snapshot.snr_db &&
                std::abs(*snapshot.signal_dbfs + 60.0) < 0.1,
            "FM metrics keep the existing latest-window behavior");

    dmr_rpt::ReceiveSignalMetrics cold_metrics(-74.0);
    cold_metrics.observe_average_power(1e-6);
    snapshot = cold_metrics.snapshot();
    require(snapshot.signal_dbfs && !snapshot.noise_dbfs && !snapshot.snr_db,
            "active signal without an idle sample leaves SNR unavailable");

    dmr_rpt::DmrAdmissionTracker tracker(-74.0, 500);
    tracker.observe_average_power(1e-6, 1000);
    require(!tracker.poll(1499),
            "above-threshold signal waits for the admission timeout");
    require(tracker.poll(1500) ==
                dmr_rpt::DmrAdmissionTimeoutReason::NoReliableSync,
            "above-threshold signal without DMR sync identifies SYNC failure");
    require(!tracker.poll(1600),
            "one RF signal episode reports only one admission timeout");

    tracker.observe_average_power(1e-12, 1700);
    tracker.observe_average_power(1e-6, 1800);
    tracker.mark_sync();
    require(tracker.poll(2300) ==
                dmr_rpt::DmrAdmissionTimeoutReason::MissingLinkControl,
            "reliable sync without a voice header identifies LC failure");

    tracker.observe_average_power(1e-12, 2500);
    tracker.observe_average_power(1e-6, 2600);
    tracker.mark_admitted();
    require(!tracker.poll(3200),
            "an admitted signal does not produce a false rejection");

    tracker.observe_average_power(1e-12, 3400);
    tracker.observe_average_power(1e-6, 3500);
    require(tracker.mark_failure() && !tracker.mark_failure(),
            "a decoded rejection is reported once per RF signal episode");
}

void test_signal_reject_console_format()
{
    const std::vector<std::string> bodies =
        dmr_rpt::format_signal_reject_console_bodies(
            {"SYNC", "no_sync"});
    require(bodies.size() == 2U && bodies[0] == "Drop S=SYNC" &&
                bodies[1] == "R=no_sync",
            "signal rejection identifies the failed step and reason");

    const std::vector<std::string> lines =
        dmr_rpt::format_console_lines(bodies);
    require(lines.size() == 2U,
            "signal rejection uses the registered continuation layout");
    for (const std::string& line : lines) {
        require(line.size() <= 40U,
                "signal rejection lines stay within 40 ASCII bytes");
    }

    bool rejected_unknown_stage = false;
    try {
        (void)dmr_rpt::format_signal_reject_console_bodies(
            {"UNREGISTERED", "unknown"});
    } catch (const std::invalid_argument&) {
        rejected_unknown_stage = true;
    }
    require(rejected_unknown_stage,
            "unregistered rejection stages cannot reach the console");
}

void set_sync_pattern(dmr_rpt::RawDmrBurst& burst, std::uint64_t pattern)
{
    for (std::size_t index = 0; index < 24U; ++index) {
        const unsigned shift = static_cast<unsigned>((23U - index) * 2U);
        burst[54U + index] = static_cast<std::uint8_t>(
            (pattern >> shift) & 0x03U);
    }
}

void test_dmr_burst_classification_and_latency(
    const std::filesystem::path& vector_root)
{
    dmr_rpt::RawDmrBurst burst{};
    set_sync_pattern(burst, 0xD5D7F77FD757ULL);
    dmr_rpt::DmrBurstSyncObservation observation =
        dmr_rpt::inspect_dmr_burst_sync(burst);
    require(observation.valid &&
                observation.kind == dmr_rpt::DmrBurstSyncKind::Data &&
                observation.bit_errors == 0,
            "MS data sync is classified as DMR data");

    set_sync_pattern(burst, 0x7DFFD5F55D5FULL);
    observation = dmr_rpt::inspect_dmr_burst_sync(burst);
    require(observation.valid &&
                observation.kind == dmr_rpt::DmrBurstSyncKind::Voice &&
                observation.direct_slot == 2,
            "direct voice sync identifies slot 2");

    set_sync_pattern(burst, 0xD5D7F77FD757ULL ^ 0x3FULL);
    require(dmr_rpt::inspect_dmr_burst_sync(burst).valid,
            "six sync bit errors remain acceptable on the real RF path");
    set_sync_pattern(burst, 0xD5D7F77FD757ULL ^ 0x7FULL);
    require(!dmr_rpt::inspect_dmr_burst_sync(burst).valid,
            "seven sync bit errors are rejected");

    burst.fill(0U);
    require(!dmr_rpt::inspect_dmr_burst_sync(burst).valid,
            "an FM-like non-DMR burst is not classified as DMR");
    require(dmr_rpt::relay_start_latency_within_limit(1000, 1500),
            "500 ms relay start latency meets the limit");
    require(!dmr_rpt::relay_start_latency_within_limit(1000, 1501),
            "relay start latency above 500 ms fails the limit");
    require(dmr_rpt::direct_relay_clock_samples_required(
                dmr_rpt::kDirectModeFrameDibits, 2) == 0 &&
                dmr_rpt::direct_relay_clock_samples_required(
                    dmr_rpt::kDirectModeFrameDibits, 1) == 0,
            "single-stream TX startup prefill does not wait for RX clock");
    require(dmr_rpt::direct_relay_clock_samples_required(
                dmr_rpt::kDirectModeFrameDibits, 0) == 2880,
            "paced TX consumes exactly one 48 kHz clock frame afterward");
    require(dmr_rpt::kSingleStreamTxStartupPrefillFrames *
                dmr_rpt::kDirectModeFrameDurationMs <
                dmr_rpt::kMaximumRelayStartLatencyMs,
            "single-stream TX scheduling reserve stays below latency limit");

    const std::filesystem::path dibit_path = vector_root /
        "direct_voice_group_slot1_001" / "input.dibits";
    std::ifstream input(dibit_path, std::ios::binary);
    require(static_cast<bool>(input),
            "real 828S dibit vector is available for classification");
    std::vector<std::uint8_t> captured(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    require(captured.size() % dmr_rpt::kDmrBurstDibits == 0U,
            "real 828S dibit vector contains complete bursts");
    std::size_t data_syncs = 0;
    std::size_t voice_syncs = 0;
    for (std::size_t offset = 0; offset < captured.size();
         offset += dmr_rpt::kDmrBurstDibits) {
        const dmr_rpt::DmrBurstSyncObservation captured_sync =
            dmr_rpt::inspect_dmr_burst_sync(
                captured.data() + offset, dmr_rpt::kDmrBurstDibits);
        data_syncs += captured_sync.valid &&
            captured_sync.kind == dmr_rpt::DmrBurstSyncKind::Data;
        voice_syncs += captured_sync.valid &&
            captured_sync.kind == dmr_rpt::DmrBurstSyncKind::Voice;
    }
    require(data_syncs > 0U && voice_syncs > 0U,
            "real 828S capture distinguishes DMR data and voice bursts");
}

void test_recording_contract()
{
    dmr_rpt::RecordingMetadata metadata;
    metadata.mode = dmr_rpt::RecordingMode::DmrRelay;
    metadata.source_id = 100103;
    metadata.destination_id = 2001;
    metadata.color_code = 1;
    metadata.slot = 2;
    const std::string filename = dmr_rpt::format_recording_filename(metadata);
    require(filename.find("_dmr_relay_") != std::string::npos,
            "recording filename identifies DMR relay mode");
    require(filename.find("_src100103_") != std::string::npos,
            "recording filename includes source ID");
    require(filename.find("_dst2001_") != std::string::npos,
            "recording filename includes destination ID");
    require(filename.find("_cc1_ts2.mp3") != std::string::npos,
            "recording filename includes color code and timeslot");
    require(dmr_rpt::to_string(dmr_rpt::RecordingMode::DmrDirect) ==
                "dmr_direct",
            "recording contract names DMR direct mode");
    require(dmr_rpt::to_string(dmr_rpt::RecordingMode::FmRelay) ==
                "fm_relay",
            "recording contract names FM relay mode");

    dmr_rpt::DmrBurstDibits burst {};
    for (std::size_t index = 0; index < burst.size(); ++index) {
        burst[index] = static_cast<std::uint8_t>(index);
    }
    const dmr_rpt::AmbeBurstFrames frames =
        dmr_rpt::extract_ambe_frames(burst);
    for (std::size_t index = 0; index < 36U; ++index) {
        require(frames[0][index] == burst[index],
                "first AMBE frame mapping is exact");
        require(frames[2][index] == burst[index + 96U],
                "third AMBE frame mapping is exact");
    }
    for (std::size_t index = 0; index < 18U; ++index) {
        require(frames[1][index] == burst[index + 36U],
                "second AMBE frame first half mapping is exact");
        require(frames[1][index + 18U] == burst[index + 78U],
                "second AMBE frame second half mapping is exact");
    }

    const std::array<std::uint8_t, 9> packed =
        dmr_rpt::pack_ambe_frame(frames[0]);
    require(packed.size() == 9U, "AMBE 36 dibits pack into 9 bytes");

    dmr_rpt::AmbeRecordingHeader header;
    header.source_id = 100103;
    header.destination_id = 100102;
    header.repeater_id = 9001;
    header.average_rssi_millidbfs = -74250;
    header.latitude_e7 = 311234567;
    header.longitude_e7 = 1219876543;
    header.duration_ms = 600;
    header.ambe_frame_count = 3;
    header.payload_size = 27;
    header.payload_crc32 = 0x12345678U;
    const std::vector<std::uint8_t> serialized =
        dmr_rpt::serialize_ambe_header(header);
    require(serialized.size() == 96U,
            "AMBE recording header is fixed at 96 bytes");
    const dmr_rpt::AmbeRecordingHeader parsed =
        dmr_rpt::parse_ambe_header(serialized);
    require(parsed.feature == "DMR-RPT-AMBE-RECORDING-V1-000001" &&
                parsed.source_id == header.source_id &&
                parsed.destination_id == header.destination_id &&
                parsed.repeater_id == header.repeater_id &&
                parsed.average_rssi_millidbfs == header.average_rssi_millidbfs &&
                parsed.latitude_e7 == header.latitude_e7 &&
                parsed.longitude_e7 == header.longitude_e7,
            "AMBE header preserves routing, signal and location metadata");

    std::vector<std::uint8_t> invalid = serialized;
    invalid[32] = 2U;
    bool rejected = false;
    try {
        dmr_rpt::parse_ambe_header(invalid);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "unsupported AMBE recording version is rejected");

    const std::filesystem::path spool = unique_temp_dir();
    dmr_rpt::RemoteVoiceConfig remote_config;
    remote_config.enabled = true;
    remote_config.spool_directory = spool;
    metadata.started_at = std::chrono::system_clock::now();
    metadata.repeater_id = 9001;
    metadata.latitude_e7 = 311234567;
    metadata.longitude_e7 = 1219876543;
    dmr_rpt::AmbeRecordingWriter writer;
    writer.start(metadata, remote_config);
    writer.observe_rssi(-60.0);
    writer.observe_rssi(-80.0);
    const std::filesystem::path ambe_path = writer.finish(123);
    std::ifstream ambe_input(ambe_path, std::ios::binary);
    const std::vector<std::uint8_t> ambe_bytes(
        (std::istreambuf_iterator<char>(ambe_input)),
        std::istreambuf_iterator<char>());
    const dmr_rpt::AmbeRecordingHeader recorded =
        dmr_rpt::parse_ambe_header(ambe_bytes);
    const double expected_average_dbfs = 10.0 * std::log10(
        (std::pow(10.0, -6.0) + std::pow(10.0, -8.0)) / 2.0);
    require(recorded.ambe_frame_count == 0U &&
                recorded.payload_size == 0U &&
                recorded.average_rssi_millidbfs == static_cast<std::int32_t>(
                    std::llround(expected_average_dbfs * 1000.0)),
            "AMBE header stores the linear-power average RSSI window");
    std::error_code cleanup_error;
    std::filesystem::remove_all(spool, cleanup_error);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 3) {
            throw std::runtime_error("usage: repeater_contract_tests <config> <vector-root>");
        }
        const std::filesystem::path config_path = argv[1];
        const std::filesystem::path vector_root = argv[2];
        test_config(config_path);
        const dmr_rpt::RepeaterConfig config = dmr_rpt::load_config_file(config_path);
        const dmr_rpt::ValidatedConfig validated = dmr_rpt::validate_config(config);
        test_router_and_safe(validated.config);
        test_receive_agc();
        test_rx_signal_calibration();
        test_ctcss_detector();
        test_io(validated.config.io_status);
        test_audit(validated.config.logging);
        test_rf_reinit(validated.rf);
        test_manifest(vector_root);
        test_call_console_format();
        test_receive_signal_metrics();
        test_signal_reject_console_format();
        test_dmr_burst_classification_and_latency(vector_root);
        test_recording_contract();
        std::cout << "repeater contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "repeater_contract_tests: " << error.what() << '\n';
        return 1;
    }
}
