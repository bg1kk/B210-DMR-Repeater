// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/config.h"

#include "dmr_rpt/analog_fm.h"
#include "dmr_rpt/sha256.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dmr_rpt {
namespace {

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_copy(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

bool is_b210_uhd_device(const std::string& device)
{
    bool b200_type_found = false;
    std::istringstream fields(device);
    std::string field;
    while (std::getline(fields, field, ',')) {
        field = trim_copy(std::move(field));
        const std::size_t separator = field.find('=');
        if (separator == std::string::npos || separator == 0U ||
            separator + 1U >= field.size()) {
            return false;
        }
        const std::string key = lower_copy(trim_copy(field.substr(0, separator)));
        const std::string value =
            lower_copy(trim_copy(field.substr(separator + 1U)));
        if (key.empty() || value.empty()) {
            return false;
        }
        if (key == "type") {
            if (value != "b200" || b200_type_found) {
                return false;
            }
            b200_type_found = true;
        }
    }
    return b200_type_found;
}

const YAML::Node require_child(const YAML::Node& node,
                              const char* key,
                              const std::string& context)
{
    const YAML::Node child = node[key];
    if (!child) {
        throw ConfigError(context + "." + key + " is required");
    }
    return child;
}

void require_map(const YAML::Node& node, const std::string& context)
{
    if (!node || !node.IsMap()) {
        throw ConfigError(context + " must be a map");
    }
}

void require_sequence(const YAML::Node& node, const std::string& context)
{
    if (!node || !node.IsSequence()) {
        throw ConfigError(context + " must be a sequence");
    }
}

std::int64_t get_i64(const YAML::Node& node,
                     const char* key,
                     const std::string& context)
{
    try {
        return require_child(node, key, context).as<std::int64_t>();
    } catch (const YAML::Exception& error) {
        throw ConfigError(context + "." + key + " must be an integer: " + error.what());
    }
}

int get_int(const YAML::Node& node, const char* key, const std::string& context)
{
    const auto value = get_i64(node, key, context);
    if (value < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        throw ConfigError(context + "." + key + " is outside int range");
    }
    return static_cast<int>(value);
}

int get_int_default(const YAML::Node& node, const char* key, int fallback,
                    const std::string& context)
{
    if (!node[key]) {
        return fallback;
    }
    return get_int(node, key, context);
}

std::uint32_t get_dmr_id(const YAML::Node& node,
                         const char* key,
                         const std::string& context)
{
    const auto value = get_i64(node, key, context);
    if (value <= 0 || value > 0xFFFFFF) {
        throw ConfigError(context + "." + key + " must be in 1..16777215");
    }
    return static_cast<std::uint32_t>(value);
}

bool get_bool(const YAML::Node& node,
              const char* key,
              const std::string& context)
{
    try {
        return require_child(node, key, context).as<bool>();
    } catch (const YAML::Exception& error) {
        throw ConfigError(context + "." + key + " must be boolean: " + error.what());
    }
}

bool get_bool_default(const YAML::Node& node,
                      const char* key,
                      bool fallback)
{
    if (!node[key]) {
        return fallback;
    }
    return node[key].as<bool>();
}

std::string get_string(const YAML::Node& node,
                       const char* key,
                       const std::string& context)
{
    try {
        return require_child(node, key, context).as<std::string>();
    } catch (const YAML::Exception& error) {
        throw ConfigError(context + "." + key + " must be a string: " + error.what());
    }
}

std::string get_string_default(const YAML::Node& node,
                               const char* key,
                               const std::string& fallback)
{
    if (!node[key]) {
        return fallback;
    }
    return node[key].as<std::string>();
}

std::int32_t parse_decimal_tenths_exact(const YAML::Node& node,
                                        const std::string& context)
{
    if (!node || !node.IsScalar()) {
        throw ConfigError(context + " must be a scalar decimal");
    }
    std::string text = node.Scalar();
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), text.end());
    if (text.empty()) {
        throw ConfigError(context + " must not be empty");
    }

    int sign = 1;
    std::size_t pos = 0;
    if (text[pos] == '+' || text[pos] == '-') {
        sign = text[pos] == '-' ? -1 : 1;
        ++pos;
    }
    if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos]))) {
        throw ConfigError(context + " must be a decimal number");
    }

    std::int64_t whole = 0;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
        whole = whole * 10 + (text[pos] - '0');
        ++pos;
    }

    int tenths = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            tenths = text[pos] - '0';
            ++pos;
        }
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            if (text[pos] != '0') {
                throw ConfigError(context + " must use 0.1 precision");
            }
            ++pos;
        }
    }
    if (pos != text.size()) {
        throw ConfigError(context + " must be a plain decimal number");
    }

    const std::int64_t value = sign * (whole * 10 + tenths);
    if (value < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
        value > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        throw ConfigError(context + " is outside fixed-point range");
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t get_gain_tenths(const YAML::Node& node,
                             const char* key,
                             const std::string& context)
{
    return parse_decimal_tenths_exact(require_child(node, key, context),
                                      context + "." + key);
}

std::int32_t get_tenths_default(const YAML::Node& node,
                                const char* key,
                                std::int32_t fallback,
                                const std::string& context)
{
    if (!node[key]) {
        return fallback;
    }
    return parse_decimal_tenths_exact(node[key], context + "." + key);
}

DmrProfile parse_profile(const std::string& text)
{
    const std::string value = lower_copy(text);
    if (value == "direct_lab") {
        return DmrProfile::DirectLab;
    }
    if (value == "t1") {
        return DmrProfile::T1;
    }
    if (value == "t2") {
        return DmrProfile::T2;
    }
    return DmrProfile::Unsupported;
}

CallType parse_call_type(const std::string& text)
{
    const std::string value = lower_copy(text);
    if (value == "group") {
        return CallType::Group;
    }
    if (value == "private") {
        return CallType::Private;
    }
    if (value == "all_call") {
        return CallType::AllCall;
    }
    return CallType::Unknown;
}

RfEndpointConfig parse_endpoint(const YAML::Node& node, const std::string& context)
{
    require_map(node, context);
    RfEndpointConfig endpoint;
    endpoint.channel = get_int(node, "channel", context);
    endpoint.frequency_hz = get_i64(node, "frequency_hz", context);
    endpoint.lo_offset_hz = get_i64(node, "lo_offset_hz", context);
    endpoint.gain_tenths_db = get_gain_tenths(node, "gain_db", context);
    endpoint.bandwidth_hz = get_i64(node, "bandwidth_hz", context);
    endpoint.antenna = get_string(node, "antenna", context);
    return endpoint;
}

ReceiveAgcConfig parse_receive_agc(const YAML::Node& radio)
{
    ReceiveAgcConfig agc;
    const YAML::Node node = radio["receive_agc"];
    if (!node) {
        return agc;
    }
    require_map(node, "radio.receive_agc");
    agc.enabled = get_bool_default(node, "enabled", agc.enabled);
    agc.target_tenths_dbfs = get_tenths_default(
        node, "target_dbfs", agc.target_tenths_dbfs, "radio.receive_agc");
    agc.minimum_gain_tenths_db = get_tenths_default(
        node, "min_gain_db", agc.minimum_gain_tenths_db, "radio.receive_agc");
    agc.maximum_gain_tenths_db = get_tenths_default(
        node, "max_gain_db", agc.maximum_gain_tenths_db, "radio.receive_agc");
    agc.attack_tenths_db_per_second = get_tenths_default(
        node, "attack_db_per_second", agc.attack_tenths_db_per_second,
        "radio.receive_agc");
    agc.release_tenths_db_per_second = get_tenths_default(
        node, "release_db_per_second", agc.release_tenths_db_per_second,
        "radio.receive_agc");
    return agc;
}

ReceiveGainControlConfig parse_receive_gain_control(const YAML::Node& radio)
{
    ReceiveGainControlConfig gain_control;
    const YAML::Node node = radio["receive_gain_control"];
    if (!node) {
        return gain_control;
    }
    require_map(node, "radio.receive_gain_control");
    gain_control.high_gain_tenths_db = get_tenths_default(
        node, "high_gain_db", gain_control.high_gain_tenths_db,
        "radio.receive_gain_control");
    gain_control.low_gain_tenths_db = get_tenths_default(
        node, "low_gain_db", gain_control.low_gain_tenths_db,
        "radio.receive_gain_control");
    gain_control.default_mode = get_string_default(
        node, "default_mode", gain_control.default_mode);
    const YAML::Node automatic = node["automatic_switching"];
    if (automatic) {
        require_map(automatic, "radio.receive_gain_control.automatic_switching");
        gain_control.automatic_switching.enabled = get_bool_default(
            automatic, "enabled", gain_control.automatic_switching.enabled);
        gain_control.automatic_switching.high_to_low_threshold_dbm =
            get_int_default(
                automatic, "high_to_low_threshold_dbm",
                gain_control.automatic_switching.high_to_low_threshold_dbm,
                "radio.receive_gain_control.automatic_switching");
        gain_control.automatic_switching.low_to_high_threshold_dbm =
            get_int_default(
                automatic, "low_to_high_threshold_dbm",
                gain_control.automatic_switching.low_to_high_threshold_dbm,
                "radio.receive_gain_control.automatic_switching");
    }
    return gain_control;
}

double get_double(const YAML::Node& node, const char* key,
                  const std::string& context)
{
    try {
        const double value = require_child(node, key, context).as<double>();
        if (!std::isfinite(value)) {
            throw ConfigError(context + "." + key + " must be finite");
        }
        return value;
    } catch (const YAML::Exception& error) {
        throw ConfigError(context + "." + key + " must be a number: " +
                          error.what());
    }
}

RxSignalCalibrationCurve parse_rx_calibration_curve(const YAML::Node& node,
                                                     const std::string& context)
{
    RxSignalCalibrationCurve curve;
    if (!node) {
        return curve;
    }
    require_map(node, context);
    if (node["rx_gain_tenths_db"]) {
        curve.rx_gain_tenths_db = get_int(node, "rx_gain_tenths_db", context);
    }
    const YAML::Node points = node["points"];
    if (points) {
        require_sequence(points, context + ".points");
        for (std::size_t index = 0; index < points.size(); ++index) {
            const YAML::Node item = points[index];
            const std::string point_context = context + ".points[" +
                std::to_string(index) + "]";
            require_map(item, point_context);
            RxSignalCalibrationPoint point;
            point.input_dbm = get_int(item, "input_dbm", point_context);
            point.measured_dbfs = get_double(item, "measured_dbfs", point_context);
            point.snr_db = get_double(item, "snr_db", point_context);
            point.calibrated_at_utc = get_string_default(
                item, "calibrated_at_utc", "");
            curve.points.push_back(std::move(point));
        }
    }
    const YAML::Node fit = node["fit"];
    if (fit && fit["segments"]) {
        require_sequence(fit["segments"], context + ".fit.segments");
        for (std::size_t index = 0; index < fit["segments"].size(); ++index) {
            const YAML::Node item = fit["segments"][index];
            const std::string segment_context = context + ".fit.segments[" +
                std::to_string(index) + "]";
            require_map(item, segment_context);
            curve.fit_segments.push_back({
                get_double(item, "measured_dbfs_low", segment_context),
                get_double(item, "measured_dbfs_high", segment_context),
                get_double(item, "slope_dbm_per_dbfs", segment_context),
                get_double(item, "intercept_dbm", segment_context)});
        }
    }
    return curve;
}

RxSignalCalibrationConfig parse_rx_signal_calibration(const YAML::Node& radio)
{
    RxSignalCalibrationConfig calibration;
    const YAML::Node root = radio["rx_signal_calibration"];
    if (!root) {
        return calibration;
    }
    require_map(root, "radio.rx_signal_calibration");
    for (int channel = 0; channel < 2; ++channel) {
        const std::string name = "rx" + std::to_string(channel + 1);
        const YAML::Node receiver = root[name];
        if (!receiver) {
            continue;
        }
        require_map(receiver, "radio.rx_signal_calibration." + name);
        const std::size_t index = static_cast<std::size_t>(channel);
        calibration.low[index] = parse_rx_calibration_curve(
            receiver["low"], "radio.rx_signal_calibration." + name + ".low");
        calibration.high[index] = parse_rx_calibration_curve(
            receiver["high"], "radio.rx_signal_calibration." + name + ".high");
    }
    return calibration;
}

AnalogFmFallbackConfig parse_analog_fm(const YAML::Node& node,
                                       const std::string& context)
{
    require_map(node, context);
    AnalogFmFallbackConfig fallback;
    fallback.enabled = get_bool(node, "enabled", context);
    const YAML::Node rx = require_child(node, "rx", context);
    require_map(rx, context + ".rx");
    fallback.rx.channel = get_int(rx, "channel", context + ".rx");
    fallback.rx.gain_tenths_db = get_gain_tenths(rx, "gain_db", context + ".rx");
    fallback.rx.bandwidth_hz = get_i64(rx, "bandwidth_hz", context + ".rx");
    fallback.rx.antenna = get_string(rx, "antenna", context + ".rx");

    const YAML::Node fm = require_child(node, "fm", context);
    require_map(fm, context + ".fm");
    fallback.fm.max_deviation_hz = get_int(fm, "max_deviation_hz", context + ".fm");
    fallback.fm.audio_bandwidth_hz = get_int(fm, "audio_bandwidth_hz", context + ".fm");
    fallback.fm.squelch_tenths_dbfs =
        get_tenths_default(fm, "squelch_db", -740, context + ".fm");

    const YAML::Node ctcss = require_child(node, "ctcss", context);
    require_map(ctcss, context + ".ctcss");
    fallback.ctcss.required = get_bool_default(ctcss, "required", true);
    fallback.ctcss.tone_tenths_hz = get_tenths_default(ctcss, "tone_hz", 1230,
                                                       context + ".ctcss");
    fallback.ctcss.minimum_confidence_tenths_db =
        get_tenths_default(ctcss, "minimum_confidence_db", 120,
                           context + ".ctcss");
    fallback.ctcss.minimum_detect_ms =
        get_int(ctcss, "minimum_detect_ms", context + ".ctcss");
    fallback.ctcss.release_hold_ms =
        get_int(ctcss, "release_hold_ms", context + ".ctcss");
    fallback.dmr_idle_guard_ms = get_int(node, "dmr_idle_guard_ms", context);

    const YAML::Node dmr_tx = require_child(node, "dmr_tx", context);
    require_map(dmr_tx, context + ".dmr_tx");
    fallback.dmr_tx.slot = get_int(dmr_tx, "slot", context + ".dmr_tx");
    fallback.dmr_tx.source_id = get_dmr_id(dmr_tx, "source_id", context + ".dmr_tx");
    fallback.dmr_tx.destination_id = get_dmr_id(dmr_tx, "destination_id", context + ".dmr_tx");
    fallback.dmr_tx.call_type =
        parse_call_type(get_string(dmr_tx, "call_type", context + ".dmr_tx"));
    fallback.dmr_tx.color_code = get_int(dmr_tx, "color_code", context + ".dmr_tx");
    return fallback;
}

ChannelProfile parse_channel_profile(const YAML::Node& node, std::size_t index)
{
    const std::string context = "channel_profiles[" + std::to_string(index) + "]";
    require_map(node, context);
    ChannelProfile profile;
    profile.id = get_string(node, "id", context);
    profile.dmr_rx = parse_endpoint(require_child(node, "dmr_rx", context),
                                    context + ".dmr_rx");
    profile.dmr_tx = parse_endpoint(require_child(node, "dmr_tx", context),
                                    context + ".dmr_tx");
    profile.analog_fm_fallback =
        parse_analog_fm(require_child(node, "analog_fm_fallback", context),
                        context + ".analog_fm_fallback");
    return profile;
}

std::vector<std::uint32_t> parse_id_list(const YAML::Node& node)
{
    std::vector<std::uint32_t> result;
    if (!node) {
        return result;
    }
    if (!node.IsSequence()) {
        throw ConfigError("routing whitelist fields must be sequences");
    }
    for (const YAML::Node& item : node) {
        const auto value = item.as<std::int64_t>();
        if (value <= 0 || value > 0xFFFFFF) {
            throw ConfigError("routing whitelist IDs must be in 1..16777215");
        }
        result.push_back(static_cast<std::uint32_t>(value));
    }
    return result;
}

bool ascii_nonempty(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x21U && ch <= 0x7eU;
    });
}

void validate_channel_number(int channel, const std::string& context)
{
    if (channel != 0 && channel != 1) {
        throw ConfigError(context + " channel must be 0 or 1");
    }
}

void validate_frequency(std::int64_t frequency_hz,
                        const RadioConfig& radio,
                        const std::string& context)
{
    if (frequency_hz < radio.operating_frequency_min_hz ||
        frequency_hz > radio.operating_frequency_max_hz) {
        throw ConfigError(context + " is outside operating range");
    }
}

void validate_endpoint(const RfEndpointConfig& endpoint,
                       const RadioConfig& radio,
                       const std::string& context)
{
    validate_channel_number(endpoint.channel, context);
    validate_frequency(endpoint.frequency_hz, radio, context + ".frequency_hz");
    if (endpoint.bandwidth_hz <= 0) {
        throw ConfigError(context + ".bandwidth_hz must be positive");
    }
    if (endpoint.antenna.empty()) {
        throw ConfigError(context + ".antenna must not be empty");
    }
    const std::int64_t hardware_center = endpoint.frequency_hz + endpoint.lo_offset_hz;
    if (hardware_center <= 0) {
        throw ConfigError(context + " hardware center frequency must be positive");
    }
    const std::int64_t half_rate = radio.sample_rate_hz / 2;
    if (std::llabs(endpoint.lo_offset_hz) + 15000 >= half_rate) {
        throw ConfigError(context + ".lo_offset_hz leaves insufficient sample bandwidth");
    }
}

void validate_io_status(const IoStatusConfig& io)
{
    if (!io.enabled) {
        return;
    }
    if (io.gpio_bank.empty()) {
        throw ConfigError("io_status.gpio_bank must not be empty");
    }
    if (io.active_level != "low" || io.idle_level != "high") {
        throw ConfigError("io_status must be low-active and high-idle");
    }
    if (io.rx_release_delay_ms < 0 || io.tx_release_delay_ms < 0) {
        throw ConfigError("io_status release delays must be non-negative");
    }
    if (io.pins.size() != 4U) {
        throw ConfigError("io_status.pins must describe IO0..IO3");
    }
    std::set<int> used_io;
    for (const IoPinConfig& pin : io.pins) {
        if (pin.io < 0 || pin.io > 3) {
            throw ConfigError("io_status pin " + pin.logical_name + " must use IO0..IO3");
        }
        if (!used_io.insert(pin.io).second) {
            throw ConfigError("io_status pin " + std::to_string(pin.io) + " is duplicated");
        }
        if (pin.rx_channel && pin.tx_channel) {
            throw ConfigError("io_status pin " + pin.logical_name + " maps both RX and TX");
        }
        if (!pin.rx_channel && !pin.tx_channel) {
            throw ConfigError("io_status pin " + pin.logical_name + " has no channel mapping");
        }
        if (pin.rx_channel && (*pin.rx_channel < 0 || *pin.rx_channel > 1)) {
            throw ConfigError("io_status " + pin.logical_name + " RX channel must be 0 or 1");
        }
        if (pin.tx_channel && (*pin.tx_channel < 0 || *pin.tx_channel > 1)) {
            throw ConfigError("io_status " + pin.logical_name + " TX channel must be 0 or 1");
        }
    }
}

IoStatusConfig parse_io_status(const YAML::Node& root)
{
    const YAML::Node node = require_child(root, "io_status", "root");
    require_map(node, "io_status");
    IoStatusConfig io;
    io.enabled = get_bool_default(node, "enabled", true);
    io.gpio_bank = get_string_default(node, "gpio_bank", "FP0");
    io.active_level = get_string_default(node, "active_level", "low");
    io.idle_level = get_string_default(node, "idle_level", "high");
    io.rx_release_delay_ms = get_int(node, "rx_release_delay_ms", "io_status");
    io.tx_release_delay_ms = get_int(node, "tx_release_delay_ms", "io_status");
    const YAML::Node pins = require_child(node, "pins", "io_status");
    require_map(pins, "io_status.pins");
    for (auto it = pins.begin(); it != pins.end(); ++it) {
        IoPinConfig pin;
        pin.logical_name = it->first.as<std::string>();
        const YAML::Node value = it->second;
        pin.io = get_int(value, "io", "io_status.pins." + pin.logical_name);
        if (value["rx_channel"]) {
            pin.rx_channel = value["rx_channel"].as<int>();
        }
        if (value["tx_channel"]) {
            pin.tx_channel = value["tx_channel"].as<int>();
        }
        io.pins.push_back(pin);
    }
    return io;
}

RepeaterConfig parse_root(const YAML::Node& root)
{
    require_map(root, "root");
    RepeaterConfig config;
    config.contract_versions = {
        {"RF", "0.12.4"},
        {"AIR", "0.6.2"},
        {"RPT", "0.7.0"},
        {"AUDIO", "0.3.4"},
        {"DATA", "0.3.8"},
        {"NET", "0.4.6"},
        {"SAFE", "0.4.5"},
        {"UDP", "0.12.3"},
        {"CAL", "0.3.1"},
        {"AFM", "0.2.5"},
        {"LOG", "0.3.2"},
        {"IO", "0.1.0"},
    };
    config.version = get_int(root, "version", "root");

    const YAML::Node radio = require_child(root, "radio", "root");
    require_map(radio, "radio");
    config.radio.uhd_device = get_string(radio, "uhd_device", "radio");
    config.radio.operating_frequency_min_hz =
        get_i64(radio, "operating_frequency_min_hz", "radio");
    config.radio.operating_frequency_max_hz =
        get_i64(radio, "operating_frequency_max_hz", "radio");
    config.radio.frequency_step_hz = get_i64(radio, "frequency_step_hz", "radio");
    config.radio.gain_step_tenths_db =
        get_gain_tenths(radio, "gain_step_db", "radio");
    config.radio.sample_rate_hz = get_i64(radio, "sample_rate_hz", "radio");
    config.radio.active_channel_profile_id =
        get_string(radio, "active_channel_profile_id", "radio");
    config.radio.receive_agc = parse_receive_agc(radio);
    config.radio.receive_gain_control = parse_receive_gain_control(radio);
    config.radio.rx_signal_calibration = parse_rx_signal_calibration(radio);

    config.io_status = parse_io_status(root);

    const YAML::Node profiles = require_child(root, "channel_profiles", "root");
    require_sequence(profiles, "channel_profiles");
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        config.channel_profiles.push_back(parse_channel_profile(profiles[index], index));
    }

    const YAML::Node dmr = require_child(root, "dmr", "root");
    require_map(dmr, "dmr");
    config.dmr.profile = parse_profile(get_string(dmr, "profile", "dmr"));
    config.dmr.receive_squelch_tenths_dbfs =
        get_tenths_default(dmr, "receive_squelch_dbfs", -740, "dmr");
    if (dmr["receive_inactivity_timeout_ms"]) {
        config.dmr.receive_inactivity_timeout_ms =
            get_int(dmr, "receive_inactivity_timeout_ms", "dmr");
    }
    config.dmr.repeater_id = get_dmr_id(dmr, "repeater_id", "dmr");
    const YAML::Node local_ptt = require_child(dmr, "local_ptt_tx", "dmr");
    require_map(local_ptt, "dmr.local_ptt_tx");
    config.dmr.local_ptt_tx.slot = get_int(local_ptt, "slot", "dmr.local_ptt_tx");
    config.dmr.local_ptt_tx.color_code =
        get_int(local_ptt, "color_code", "dmr.local_ptt_tx");
    config.dmr.local_ptt_tx.call_type =
        parse_call_type(get_string(local_ptt, "call_type", "dmr.local_ptt_tx"));
    config.dmr.local_ptt_tx.destination_id =
        get_dmr_id(local_ptt, "destination_id", "dmr.local_ptt_tx");

    const YAML::Node routing = require_child(root, "routing", "root");
    require_map(routing, "routing");
    config.routing.policy = get_string(routing, "policy", "routing");
    config.routing.source_id_whitelist = parse_id_list(routing["source_id_whitelist"]);
    config.routing.destination_id_whitelist =
        parse_id_list(routing["destination_id_whitelist"]);

    const YAML::Node data = require_child(root, "data", "root");
    require_map(data, "data");
    config.data.enabled = get_bool_default(data, "enabled", false);

    const YAML::Node transmit = require_child(root, "transmit", "root");
    require_map(transmit, "transmit");
    config.transmit.enabled = get_bool(transmit, "enabled", "transmit");
    config.transmit.authorization_mode =
        get_string(transmit, "authorization_mode", "transmit");
    config.transmit.require_pretransmit_confirmation =
        get_bool(transmit, "require_pretransmit_confirmation", "transmit");
    config.transmit.maximum_continuous_seconds =
        get_int(transmit, "maximum_continuous_seconds", "transmit");
    config.transmit.source_cooldown_seconds =
        get_int(transmit, "source_cooldown_seconds", "transmit");
    config.transmit.hangtime_ms = get_int(transmit, "hangtime_ms", "transmit");

    const YAML::Node audio = require_child(root, "local_audio", "root");
    require_map(audio, "local_audio");
    config.local_audio.monitor_enabled =
        get_bool(audio, "monitor_enabled", "local_audio");
    config.local_audio.input_enabled = get_bool(audio, "input_enabled", "local_audio");
    config.local_audio.backend = get_string(audio, "backend", "local_audio");
    config.local_audio.capture_device =
        get_string(audio, "capture_device", "local_audio");
    config.local_audio.playback_device =
        get_string(audio, "playback_device", "local_audio");
    config.local_audio.sample_rate_hz =
        get_int(audio, "sample_rate_hz", "local_audio");
    config.local_audio.sample_format =
        get_string(audio, "sample_format", "local_audio");
    config.local_audio.channels = get_int(audio, "channels", "local_audio");
    config.local_audio.echo_cancellation =
        get_bool(audio, "echo_cancellation", "local_audio");
    config.local_audio.recording_format =
        get_string(audio, "recording_format", "local_audio");
    config.local_audio.recording_directory =
        get_string(audio, "recording_directory", "local_audio");

    const YAML::Node remote = require_child(root, "remote_gateway", "root");
    require_map(remote, "remote_gateway");
    config.remote_gateway.enabled =
        get_bool(remote, "enabled", "remote_gateway");
    config.remote_gateway.implementation =
        get_string(remote, "implementation", "remote_gateway");
    config.remote_gateway.reject_enable =
        get_bool(remote, "reject_enable", "remote_gateway");

    const YAML::Node udp = require_child(root, "udp_control", "root");
    require_map(udp, "udp_control");
    config.udp_control.enabled = get_bool(udp, "enabled", "udp_control");
    config.udp_control.protocol = get_string(udp, "protocol", "udp_control");
    config.udp_control.bind_address =
        get_string(udp, "bind_address", "udp_control");
    config.udp_control.port = get_int(udp, "port", "udp_control");
    config.udp_control.maximum_datagram_bytes =
        get_int(udp, "maximum_datagram_bytes", "udp_control");
    const YAML::Node local_ptt_udp = require_child(udp, "local_ptt", "udp_control");
    config.udp_control.default_lease_ms =
        get_int(local_ptt_udp, "default_lease_ms", "udp_control.local_ptt");
    config.udp_control.maximum_lease_ms =
        get_int(local_ptt_udp, "maximum_lease_ms", "udp_control.local_ptt");
    config.udp_control.control_token =
        get_string_default(udp, "control_token", "");

    const YAML::Node tcp = require_child(root, "tcp_status", "root");
    require_map(tcp, "tcp_status");
    config.tcp_status.enabled = get_bool_default(tcp, "enabled", true);
    config.tcp_status.protocol =
        get_string_default(tcp, "protocol", "dmr-rpt-tcp/1");
    config.tcp_status.bind_address =
        get_string_default(tcp, "bind_address", "127.0.0.1");
    config.tcp_status.port = get_int(tcp, "port", "tcp_status");
    config.tcp_status.maximum_clients =
        get_int(tcp, "maximum_clients", "tcp_status");
    config.tcp_status.interval_ms =
        get_int(tcp, "interval_ms", "tcp_status");

    const YAML::Node remote_voice =
        require_child(root, "remote_voice", "root");
    require_map(remote_voice, "remote_voice");
    config.remote_voice.enabled =
        get_bool_default(remote_voice, "enabled", false);
    config.remote_voice.protocol =
        get_string_default(remote_voice, "protocol", "dmr-rpt-ambe/1");
    config.remote_voice.server_address =
        get_string_default(remote_voice, "server_address", "127.0.0.1");
    config.remote_voice.server_port =
        get_int(remote_voice, "server_port", "remote_voice");
    config.remote_voice.device_id =
        get_string_default(remote_voice, "device_id", "repeater-1");
    config.remote_voice.spool_directory =
        get_string_default(remote_voice, "spool_directory", "");
    config.remote_voice.connect_timeout_ms =
        get_int(remote_voice, "connect_timeout_ms", "remote_voice");
    config.remote_voice.upload_timeout_ms =
        get_int(remote_voice, "upload_timeout_ms", "remote_voice");
    config.remote_voice.latitude_e7 =
        static_cast<std::int32_t>(get_i64(
            remote_voice, "latitude_e7", "remote_voice"));
    config.remote_voice.longitude_e7 =
        static_cast<std::int32_t>(get_i64(
            remote_voice, "longitude_e7", "remote_voice"));
    config.remote_voice.feature =
        get_string_default(remote_voice, "feature",
                           "DMR-RPT-AMBE-RECORDING-V1-000001");

    const YAML::Node logging = require_child(root, "logging", "root");
    require_map(logging, "logging");
    config.logging.event_directory =
        get_string(logging, "event_directory", "logging");
    config.logging.recording_directory =
        get_string(logging, "recording_directory", "logging");
    config.logging.startup_file_prefix =
        get_string(logging, "startup_file_prefix", "logging");
    config.logging.retention_policy =
        get_string(logging, "retention_policy", "logging");
    config.logging.automatic_cleanup =
        get_bool(logging, "automatic_cleanup", "logging");
    config.logging.rotation_enabled =
        get_bool(logging, "rotation_enabled", "logging");
    config.logging.remote_archive_enabled =
        get_bool(logging, "remote_archive_enabled", "logging");
    config.logging.max_queue_events =
        get_int(logging, "max_queue_events", "logging");

    return config;
}

} // namespace

ConfigError::ConfigError(const std::string& message)
    : std::runtime_error(message)
{
}

RepeaterConfig load_config_file(const std::filesystem::path& path)
{
    try {
        return parse_root(YAML::LoadFile(path.string()));
    } catch (const YAML::Exception& error) {
        throw ConfigError("failed to parse config " + path.string() + ": " + error.what());
    }
}

RepeaterConfig load_config_string(const std::string& yaml_text)
{
    try {
        return parse_root(YAML::Load(yaml_text));
    } catch (const YAML::Exception& error) {
        throw ConfigError("failed to parse config string: " + std::string(error.what()));
    }
}

void persist_active_channel_profile(const std::filesystem::path& path,
                                    const std::string& profile_id)
{
    if (profile_id.empty()) {
        throw ConfigError("active channel profile ID must not be empty");
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
        require_map(root, "root");
        const YAML::Node radio = require_child(root, "radio", "root");
        require_map(radio, "radio");
        root["radio"]["active_channel_profile_id"] = profile_id;
    } catch (const YAML::Exception& error) {
        throw ConfigError("failed to update config " + path.string() + ": " +
                          error.what());
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << root;
    if (!emitter.good()) {
        throw ConfigError("failed to serialize config " + path.string());
    }

    const std::filesystem::path temporary_path =
        std::filesystem::path(path.string() + ".active-channel.tmp");
    const auto remove_temporary = [&temporary_path]() {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    };

    std::error_code status_error;
    const std::filesystem::file_status original_status =
        std::filesystem::status(path, status_error);
    if (status_error) {
        throw ConfigError("cannot inspect config permissions " + path.string() +
                          ": " + status_error.message());
    }

    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ConfigError("cannot create temporary config " +
                              temporary_path.string());
        }
        output << emitter.c_str() << '\n';
        output.flush();
        if (!output) {
            output.close();
            remove_temporary();
            throw ConfigError("cannot write temporary config " +
                              temporary_path.string());
        }
    }

    std::error_code permissions_error;
    std::filesystem::permissions(temporary_path, original_status.permissions(),
                                 std::filesystem::perm_options::replace,
                                 permissions_error);
    if (permissions_error) {
        remove_temporary();
        throw ConfigError("cannot preserve config permissions " + path.string() +
                          ": " + permissions_error.message());
    }

#if defined(_WIN32)
    if (!::MoveFileExW(temporary_path.wstring().c_str(), path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long error = ::GetLastError();
        remove_temporary();
        throw ConfigError("cannot replace config " + path.string() +
                          ": Windows error " + std::to_string(error));
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
    if (rename_error) {
        remove_temporary();
        throw ConfigError("cannot replace config " + path.string() + ": " +
                          rename_error.message());
    }
#endif
}

void persist_channel_profile(const std::filesystem::path& path,
                             const ChannelProfile& profile)
{
    if (profile.id.empty()) {
        throw ConfigError("channel profile ID must not be empty");
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
        require_map(root, "root");
        const YAML::Node profiles = require_child(root, "channel_profiles", "root");
        require_sequence(profiles, "channel_profiles");
        bool updated = false;
        for (std::size_t index = 0; index < profiles.size(); ++index) {
            if (get_string(profiles[index], "id", "channel_profiles") != profile.id) {
                continue;
            }
            YAML::Node stored = root["channel_profiles"][index];
            stored["dmr_rx"]["frequency_hz"] = profile.dmr_rx.frequency_hz;
            stored["dmr_rx"]["gain_db"] =
                static_cast<double>(profile.dmr_rx.gain_tenths_db) / 10.0;
            stored["dmr_tx"]["frequency_hz"] = profile.dmr_tx.frequency_hz;
            stored["dmr_tx"]["gain_db"] =
                static_cast<double>(profile.dmr_tx.gain_tenths_db) / 10.0;
            stored["analog_fm_fallback"]["enabled"] =
                profile.analog_fm_fallback.enabled;
            stored["analog_fm_fallback"]["ctcss"]["tone_hz"] =
                static_cast<double>(profile.analog_fm_fallback.ctcss.tone_tenths_hz) / 10.0;
            updated = true;
            break;
        }
        if (!updated) {
            throw ConfigError("channel profile not found: " + profile.id);
        }
    } catch (const YAML::Exception& error) {
        throw ConfigError("failed to update config " + path.string() + ": " +
                          error.what());
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << root;
    if (!emitter.good()) {
        throw ConfigError("failed to serialize config " + path.string());
    }

    const std::filesystem::path temporary_path =
        std::filesystem::path(path.string() + ".channel-profile.tmp");
    const auto remove_temporary = [&temporary_path]() {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    };
    std::error_code status_error;
    const std::filesystem::file_status original_status =
        std::filesystem::status(path, status_error);
    if (status_error) {
        throw ConfigError("cannot inspect config permissions " + path.string() +
                          ": " + status_error.message());
    }
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ConfigError("cannot create temporary config " + temporary_path.string());
        }
        output << emitter.c_str() << '\n';
        output.flush();
        if (!output) {
            output.close();
            remove_temporary();
            throw ConfigError("cannot write temporary config " + temporary_path.string());
        }
    }
    std::error_code permissions_error;
    std::filesystem::permissions(temporary_path, original_status.permissions(),
                                 std::filesystem::perm_options::replace,
                                 permissions_error);
    if (permissions_error) {
        remove_temporary();
        throw ConfigError("cannot preserve config permissions " + path.string() +
                          ": " + permissions_error.message());
    }
#if defined(_WIN32)
    if (!::MoveFileExW(temporary_path.wstring().c_str(), path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long error = ::GetLastError();
        remove_temporary();
        throw ConfigError("cannot replace config " + path.string() +
                          ": Windows error " + std::to_string(error));
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
    if (rename_error) {
        remove_temporary();
        throw ConfigError("cannot replace config " + path.string() + ": " +
                          rename_error.message());
    }
#endif
}

void persist_rx_signal_calibration(
    const std::filesystem::path& path,
    const RxSignalCalibrationConfig& calibration)
{
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
        require_map(root, "root");
        const YAML::Node radio = require_child(root, "radio", "root");
        require_map(radio, "radio");
        YAML::Node calibration_node(YAML::NodeType::Map);
        for (int channel = 0; channel < 2; ++channel) {
            YAML::Node receiver(YAML::NodeType::Map);
            const std::size_t index = static_cast<std::size_t>(channel);
            for (const auto band : {RxCalibrationBand::Low,
                                    RxCalibrationBand::High}) {
                const RxSignalCalibrationCurve& curve =
                    band == RxCalibrationBand::Low
                        ? calibration.low[index] : calibration.high[index];
                RxSignalCalibrationCurve fitted = curve;
                fit_rx_signal_calibration_curve(fitted);
                YAML::Node curve_node(YAML::NodeType::Map);
                if (fitted.rx_gain_tenths_db) {
                    curve_node["rx_gain_tenths_db"] = *fitted.rx_gain_tenths_db;
                }
                YAML::Node points(YAML::NodeType::Sequence);
                for (const RxSignalCalibrationPoint& point : fitted.points) {
                    YAML::Node value(YAML::NodeType::Map);
                    value["input_dbm"] = point.input_dbm;
                    value["measured_dbfs"] = point.measured_dbfs;
                    value["snr_db"] = point.snr_db;
                    value["calibrated_at_utc"] = point.calibrated_at_utc;
                    points.push_back(value);
                }
                curve_node["points"] = points;
                YAML::Node fit(YAML::NodeType::Map);
                fit["type"] = "piecewise_linear";
                YAML::Node segments(YAML::NodeType::Sequence);
                for (const RxSignalCalibrationSegment& segment : fitted.fit_segments) {
                    YAML::Node value(YAML::NodeType::Map);
                    value["measured_dbfs_low"] = segment.measured_dbfs_low;
                    value["measured_dbfs_high"] = segment.measured_dbfs_high;
                    value["slope_dbm_per_dbfs"] = segment.slope_dbm_per_dbfs;
                    value["intercept_dbm"] = segment.intercept_dbm;
                    segments.push_back(value);
                }
                fit["segments"] = segments;
                curve_node["fit"] = fit;
                receiver[to_string(band)] = curve_node;
            }
            calibration_node["rx" + std::to_string(channel + 1)] = receiver;
        }
        root["radio"]["rx_signal_calibration"] = calibration_node;
    } catch (const YAML::Exception& error) {
        throw ConfigError("failed to update config " + path.string() + ": " +
                          error.what());
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << root;
    if (!emitter.good()) {
        throw ConfigError("failed to serialize config " + path.string());
    }
    const std::filesystem::path temporary_path(
        path.string() + ".rx-calibration.tmp");
    std::error_code status_error;
    const std::filesystem::file_status original_status =
        std::filesystem::status(path, status_error);
    if (status_error) {
        throw ConfigError("cannot inspect config permissions " + path.string() +
                          ": " + status_error.message());
    }
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ConfigError("cannot create temporary config " +
                              temporary_path.string());
        }
        output << emitter.c_str() << '\n';
        output.flush();
        if (!output) {
            std::error_code ignored;
            output.close();
            std::filesystem::remove(temporary_path, ignored);
            throw ConfigError("cannot write temporary config " +
                              temporary_path.string());
        }
    }
    std::error_code permissions_error;
    std::filesystem::permissions(temporary_path, original_status.permissions(),
                                 std::filesystem::perm_options::replace,
                                 permissions_error);
    if (permissions_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw ConfigError("cannot preserve config permissions " + path.string() +
                          ": " + permissions_error.message());
    }
#if defined(_WIN32)
    if (!::MoveFileExW(temporary_path.wstring().c_str(), path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const unsigned long error = ::GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw ConfigError("cannot replace config " + path.string() +
                          ": Windows error " + std::to_string(error));
    }
#else
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw ConfigError("cannot replace config " + path.string() + ": " +
                          rename_error.message());
    }
#endif
}

bool is_selectable_receive_gain_mode(const std::string& mode)
{
    return mode == "high" || mode == "low";
}

bool is_receive_gain_selection_mode(const std::string& mode)
{
    return mode == "auto" || is_selectable_receive_gain_mode(mode);
}

std::int32_t receive_gain_tenths_db_for_mode(
    const ReceiveGainControlConfig& config, const std::string& mode)
{
    if (mode == "high") {
        return config.high_gain_tenths_db;
    }
    if (mode == "low") {
        return config.low_gain_tenths_db;
    }
    throw ConfigError("receive gain mode must be high or low");
}

ValidatedConfig validate_config(const RepeaterConfig& config)
{
    if (!is_b210_uhd_device(config.radio.uhd_device)) {
        throw ConfigError(
            "radio.uhd_device must be B210 UHD parameters including type=b200");
    }
    if (config.radio.operating_frequency_min_hz != 136000000 ||
        config.radio.operating_frequency_max_hz != 520000000) {
        throw ConfigError("radio operating range must be 136000000..520000000 Hz");
    }
    if (config.radio.frequency_step_hz != 1) {
        throw ConfigError("radio.frequency_step_hz must be 1");
    }
    if (config.radio.gain_step_tenths_db != 1) {
        throw ConfigError("radio.gain_step_db must be 0.1");
    }
    if (config.radio.sample_rate_hz <= 0) {
        throw ConfigError("radio.sample_rate_hz must be positive");
    }
    const ReceiveAgcConfig& agc = config.radio.receive_agc;
    if (agc.target_tenths_dbfs >= 0 || agc.target_tenths_dbfs < -1000 ||
        agc.minimum_gain_tenths_db > 0 || agc.maximum_gain_tenths_db < 0 ||
        agc.minimum_gain_tenths_db > agc.maximum_gain_tenths_db ||
        agc.attack_tenths_db_per_second <= 0 ||
        agc.release_tenths_db_per_second <= 0 ||
        agc.attack_tenths_db_per_second > 12000 ||
        agc.release_tenths_db_per_second > 12000) {
        throw ConfigError("radio.receive_agc parameters are invalid");
    }
    const ReceiveGainControlConfig& gain_control =
        config.radio.receive_gain_control;
    if (gain_control.low_gain_tenths_db != 0 ||
        gain_control.high_gain_tenths_db <= gain_control.low_gain_tenths_db ||
        (gain_control.default_mode != "configured" &&
         !is_receive_gain_selection_mode(gain_control.default_mode)) ||
        gain_control.automatic_switching.high_to_low_threshold_dbm != -70 ||
        gain_control.automatic_switching.low_to_high_threshold_dbm != -60 ||
        gain_control.automatic_switching.high_to_low_threshold_dbm >=
            gain_control.automatic_switching.low_to_high_threshold_dbm) {
        throw ConfigError("radio.receive_gain_control parameters are invalid");
    }
    for (int channel = 0; channel < 2; ++channel) {
        const std::size_t index = static_cast<std::size_t>(channel);
        for (const auto band : {RxCalibrationBand::Low,
                                RxCalibrationBand::High}) {
            const RxSignalCalibrationCurve& curve =
                band == RxCalibrationBand::Low
                    ? config.radio.rx_signal_calibration.low[index]
                    : config.radio.rx_signal_calibration.high[index];
            if (!curve.points.empty() &&
                !rx_calibration_curve_complete(curve, band)) {
                throw ConfigError("radio.rx_signal_calibration curve is incomplete or invalid");
            }
        }
    }
    if (config.channel_profiles.size() < 8U) {
        throw ConfigError("channel_profiles must contain at least 8 profiles");
    }

    std::unordered_set<std::string> ids;
    const ChannelProfile* active = nullptr;
    for (const ChannelProfile& profile : config.channel_profiles) {
        if (profile.id.empty()) {
            throw ConfigError("channel profile id must not be empty");
        }
        if (!ids.insert(profile.id).second) {
            throw ConfigError("duplicate channel profile id: " + profile.id);
        }
        validate_endpoint(profile.dmr_rx, config.radio, "profile " + profile.id + ".dmr_rx");
        validate_endpoint(profile.dmr_tx, config.radio, "profile " + profile.id + ".dmr_tx");
        validate_channel_number(profile.analog_fm_fallback.rx.channel,
                                "profile " + profile.id + ".analog_fm_fallback.rx");
        if (profile.analog_fm_fallback.enabled) {
            if (profile.analog_fm_fallback.rx.channel == profile.dmr_rx.channel) {
                throw ConfigError("profile " + profile.id +
                                  " analog FM RX channel must differ from DMR RX");
            }
            if (!profile.analog_fm_fallback.ctcss.required ||
                profile.analog_fm_fallback.ctcss.tone_tenths_hz <= 0) {
                throw ConfigError("profile " + profile.id +
                                  " analog FM requires a positive configured CTCSS tone");
            }
            if (profile.analog_fm_fallback.fm.max_deviation_hz <= 0 ||
                profile.analog_fm_fallback.fm.audio_bandwidth_hz <= 0 ||
                profile.analog_fm_fallback.fm.audio_bandwidth_hz > 3500 ||
                profile.analog_fm_fallback.fm.squelch_tenths_dbfs >= 0) {
                throw ConfigError("profile " + profile.id +
                                  " analog FM demodulation parameters are invalid");
            }
            if (profile.analog_fm_fallback.dmr_idle_guard_ms < 0 ||
                analog_fm_relay_start_latency_bound_ms(
                    profile.analog_fm_fallback) > 500) {
                throw ConfigError("profile " + profile.id +
                                  " analog FM relay start latency exceeds 500 ms");
            }
        }
        if (profile.analog_fm_fallback.dmr_tx.source_id != 9999 ||
            profile.analog_fm_fallback.dmr_tx.destination_id != 0xFFFFFFU ||
            profile.analog_fm_fallback.dmr_tx.call_type != CallType::AllCall) {
            throw ConfigError("profile " + profile.id +
                              " analog FM DMR identity must be source 9999 all-call");
        }
        if (profile.analog_fm_fallback.dmr_tx.slot != 1 &&
            profile.analog_fm_fallback.dmr_tx.slot != 2) {
            throw ConfigError("profile " + profile.id + " analog FM TX slot must be 1 or 2");
        }
        if (profile.analog_fm_fallback.dmr_tx.color_code < 0 ||
            profile.analog_fm_fallback.dmr_tx.color_code > 15) {
            throw ConfigError("profile " + profile.id + " analog FM TX color code out of range");
        }
        if (profile.id == config.radio.active_channel_profile_id) {
            active = &profile;
        }
    }
    if (active == nullptr) {
        throw ConfigError("radio.active_channel_profile_id does not match a profile");
    }

    validate_io_status(config.io_status);

    if (config.dmr.profile == DmrProfile::Unsupported) {
        throw ConfigError("dmr.profile must be direct_lab, t1, or t2");
    }
    if (config.dmr.receive_squelch_tenths_dbfs >= 0 ||
        config.dmr.receive_squelch_tenths_dbfs < -2000) {
        throw ConfigError("dmr.receive_squelch_dbfs must be in [-200.0, 0.0)");
    }
    if (config.dmr.local_ptt_tx.call_type != CallType::AllCall ||
        config.dmr.local_ptt_tx.destination_id != 0xFFFFFFU) {
        throw ConfigError("dmr.local_ptt_tx must use all_call destination 16777215");
    }
    if (config.dmr.local_ptt_tx.slot != 1 && config.dmr.local_ptt_tx.slot != 2) {
        throw ConfigError("dmr.local_ptt_tx.slot must be 1 or 2");
    }
    if (config.dmr.local_ptt_tx.color_code < 0 ||
        config.dmr.local_ptt_tx.color_code > 15) {
        throw ConfigError("dmr.local_ptt_tx.color_code must be 0..15");
    }

    if (config.routing.policy != "route_all_valid_etsi") {
        throw ConfigError("routing.policy must be route_all_valid_etsi");
    }
    if (!config.routing.source_id_whitelist.empty() ||
        !config.routing.destination_id_whitelist.empty()) {
        throw ConfigError("routing whitelists must remain empty");
    }

    if (config.data.enabled) {
        throw ConfigError("data service is disabled in this release");
    }

    if (config.transmit.authorization_mode != "preauthorized" ||
        config.transmit.require_pretransmit_confirmation) {
        throw ConfigError("transmit must be preauthorized without confirmation");
    }
    if (config.transmit.maximum_continuous_seconds != 600) {
        throw ConfigError("transmit.maximum_continuous_seconds must be 600");
    }
    if (config.transmit.source_cooldown_seconds < 30) {
        throw ConfigError("transmit.source_cooldown_seconds must be at least 30");
    }
    if (config.transmit.hangtime_ms < 0) {
        throw ConfigError("transmit.hangtime_ms must be non-negative");
    }
    if (config.dmr.receive_inactivity_timeout_ms < 180 ||
        config.dmr.receive_inactivity_timeout_ms > 5000) {
        throw ConfigError(
            "dmr.receive_inactivity_timeout_ms must be in 180..5000");
    }

    if (config.local_audio.backend != "alsa" ||
        config.local_audio.sample_rate_hz != 8000 ||
        config.local_audio.sample_format != "s16le" ||
        config.local_audio.channels != 1 ||
        config.local_audio.echo_cancellation) {
        throw ConfigError("local_audio must be ALSA 8000 Hz s16le mono without echo cancellation");
    }

    if (config.remote_gateway.enabled ||
        config.remote_gateway.implementation != "reserved" ||
        !config.remote_gateway.reject_enable) {
        throw ConfigError("remote_gateway is reserved and must remain disabled");
    }

    if (config.udp_control.protocol != "dmr-rpt-udp/1") {
        throw ConfigError("udp_control.protocol must be dmr-rpt-udp/1");
    }
    if (config.udp_control.port <= 0 || config.udp_control.port > 65535) {
        throw ConfigError("udp_control.port must be 1..65535");
    }
    if (config.udp_control.maximum_datagram_bytes > 1200) {
        throw ConfigError("udp_control.maximum_datagram_bytes must not exceed 1200");
    }
    if (config.udp_control.maximum_lease_ms > 1000 ||
        config.udp_control.default_lease_ms > config.udp_control.maximum_lease_ms) {
        throw ConfigError("udp_control local PTT lease exceeds contract maximum");
    }
    if (config.tcp_status.protocol != "dmr-rpt-tcp/1") {
        throw ConfigError("tcp_status.protocol must be dmr-rpt-tcp/1");
    }
    if (config.tcp_status.port <= 0 || config.tcp_status.port > 65535 ||
        config.tcp_status.maximum_clients <= 0 ||
        config.tcp_status.interval_ms != 1000) {
        throw ConfigError(
            "tcp_status requires a valid port, positive client limit, and 1000 ms interval");
    }
    if (config.remote_voice.protocol != "dmr-rpt-ambe/1") {
        throw ConfigError("remote_voice.protocol must be dmr-rpt-ambe/1");
    }
    if (config.remote_voice.server_port <= 0 ||
        config.remote_voice.server_port > 65535 ||
        config.remote_voice.connect_timeout_ms <= 0 ||
        config.remote_voice.upload_timeout_ms <= 0) {
        throw ConfigError("remote_voice server and timeout settings are invalid");
    }
    if (config.remote_voice.device_id.empty() ||
        config.remote_voice.feature.size() != 32U) {
        throw ConfigError(
            "remote_voice device_id must be non-empty and feature must be exactly 32 bytes");
    }
    if (config.remote_voice.latitude_e7 < -900000000 ||
        config.remote_voice.latitude_e7 > 900000000 ||
        config.remote_voice.longitude_e7 < -1800000000 ||
        config.remote_voice.longitude_e7 > 1800000000) {
        throw ConfigError("remote_voice latitude/longitude are outside WGS84 range");
    }
    if (config.remote_voice.enabled &&
        config.remote_voice.spool_directory.empty()) {
        throw ConfigError(
            "remote_voice.spool_directory is required when remote voice is enabled");
    }

    if (!ascii_nonempty(config.logging.startup_file_prefix)) {
        throw ConfigError("logging.startup_file_prefix must be non-empty ASCII");
    }
    if (config.local_audio.recording_format != "mp3") {
        throw ConfigError("local_audio.recording_format must be mp3");
    }
    if (config.logging.recording_directory.empty()) {
        throw ConfigError("logging.recording_directory must not be empty");
    }
    if (config.logging.retention_policy != "retain_forever" ||
        config.logging.automatic_cleanup ||
        config.logging.rotation_enabled ||
        config.logging.remote_archive_enabled) {
        throw ConfigError("logging must retain forever without cleanup, rotation, or archive");
    }
    if (config.logging.max_queue_events <= 0) {
        throw ConfigError("logging.max_queue_events must be positive");
    }

    ValidatedConfig validated;
    validated.config = config;
    validated.rf.active_channel_profile_id = config.radio.active_channel_profile_id;
    validated.rf.configured_channel_profile_count = config.channel_profiles.size();
    validated.rf.active_profile = *active;
    validated.rf.radio = config.radio;
    validated.rf.io_status = config.io_status;
    validated.semantic_sha256 = sha256_hex(canonical_config_summary(config));
    return validated;
}

std::string canonical_config_summary(const RepeaterConfig& config)
{
    std::ostringstream out;
    out << "version=" << config.version << '\n';
    out << "radio.uhd_device=" << config.radio.uhd_device << '\n';
    out << "radio.active=" << config.radio.active_channel_profile_id << '\n';
    out << "radio.range=" << config.radio.operating_frequency_min_hz << '-'
        << config.radio.operating_frequency_max_hz << '\n';
    out << "radio.receive_agc=" << (config.radio.receive_agc.enabled ? 1 : 0)
        << ':' << config.radio.receive_agc.target_tenths_dbfs
        << ':' << config.radio.receive_agc.minimum_gain_tenths_db
        << ':' << config.radio.receive_agc.maximum_gain_tenths_db
        << ':' << config.radio.receive_agc.attack_tenths_db_per_second
        << ':' << config.radio.receive_agc.release_tenths_db_per_second << '\n';
    out << "radio.receive_gain_control="
        << config.radio.receive_gain_control.high_gain_tenths_db << ':'
        << config.radio.receive_gain_control.low_gain_tenths_db << ':'
        << config.radio.receive_gain_control.default_mode << ':'
        << (config.radio.receive_gain_control.automatic_switching.enabled ? "true" : "false")
        << ':' << config.radio.receive_gain_control.automatic_switching
                      .high_to_low_threshold_dbm
        << ':' << config.radio.receive_gain_control.automatic_switching
                      .low_to_high_threshold_dbm << '\n';
    for (int channel = 0; channel < 2; ++channel) {
        const std::size_t index = static_cast<std::size_t>(channel);
        for (const auto band : {RxCalibrationBand::Low,
                                RxCalibrationBand::High}) {
            const RxSignalCalibrationCurve& curve =
                band == RxCalibrationBand::Low
                    ? config.radio.rx_signal_calibration.low[index]
                    : config.radio.rx_signal_calibration.high[index];
            out << "radio.rx_signal_calibration.rx" << channel + 1 << '.'
                << to_string(band) << '=';
            if (curve.rx_gain_tenths_db) {
                out << *curve.rx_gain_tenths_db;
            }
            for (const RxSignalCalibrationPoint& point : curve.points) {
                out << ':' << point.input_dbm << ',' << point.measured_dbfs
                    << ',' << point.snr_db << ',' << point.calibrated_at_utc;
            }
            out << '\n';
        }
    }
    for (const ChannelProfile& profile : config.channel_profiles) {
        out << "profile=" << profile.id
            << ",rx=" << profile.dmr_rx.channel << ':' << profile.dmr_rx.frequency_hz
            << ':' << profile.dmr_rx.lo_offset_hz << ':' << profile.dmr_rx.gain_tenths_db
            << ",tx=" << profile.dmr_tx.channel << ':' << profile.dmr_tx.frequency_hz
            << ':' << profile.dmr_tx.lo_offset_hz << ':' << profile.dmr_tx.gain_tenths_db
            << ",afm=" << (profile.analog_fm_fallback.enabled ? 1 : 0)
            << ':' << profile.analog_fm_fallback.rx.channel
            << ':' << profile.analog_fm_fallback.fm.max_deviation_hz
            << ':' << profile.analog_fm_fallback.fm.audio_bandwidth_hz
            << ':' << profile.analog_fm_fallback.fm.squelch_tenths_dbfs
            << ':' << profile.analog_fm_fallback.ctcss.tone_tenths_hz
            << ':' << profile.analog_fm_fallback.dmr_tx.slot
            << ':' << profile.analog_fm_fallback.dmr_tx.source_id
            << ':' << profile.analog_fm_fallback.dmr_tx.destination_id
            << ':' << to_string(profile.analog_fm_fallback.dmr_tx.call_type)
            << ':' << profile.analog_fm_fallback.dmr_tx.color_code << '\n';
    }
    out << "dmr.profile=" << to_string(config.dmr.profile)
        << ",squelch=" << config.dmr.receive_squelch_tenths_dbfs
        << ",receive_inactivity=" << config.dmr.receive_inactivity_timeout_ms
        << ",repeater=" << config.dmr.repeater_id << '\n';
    out << "data.enabled=" << (config.data.enabled ? 1 : 0) << '\n';
    out << "transmit.enabled=" << (config.transmit.enabled ? 1 : 0)
        << ",max=" << config.transmit.maximum_continuous_seconds
        << ",cooldown=" << config.transmit.source_cooldown_seconds
        << ",hangtime=" << config.transmit.hangtime_ms << '\n';
    out << "io.enabled=" << (config.io_status.enabled ? 1 : 0)
        << ",bank=" << config.io_status.gpio_bank << '\n';
    out << "tcp_status.enabled=" << (config.tcp_status.enabled ? 1 : 0)
        << ",bind=" << config.tcp_status.bind_address
        << ",port=" << config.tcp_status.port << '\n';
    out << "remote_voice.enabled=" << (config.remote_voice.enabled ? 1 : 0)
        << ",server=" << config.remote_voice.server_address
        << ':' << config.remote_voice.server_port
        << ",device=" << config.remote_voice.device_id << '\n';
    out << "recording.format=" << config.local_audio.recording_format
        << ",directory=" << config.logging.recording_directory.string()
        << '\n';
    for (const auto& item : config.contract_versions) {
        out << "contract." << item.first << '=' << item.second << '\n';
    }
    return out.str();
}

ChannelProfile make_lab_20260804_loopback_profile()
{
    ChannelProfile profile;
    profile.id = "lab-20260804-loopback";
    profile.dmr_rx = {1, 438500000, 100000, 500, 200000, "RX2"};
    profile.dmr_tx = {0, 438500000, 100000, 850, 200000, "TX/RX"};
    profile.analog_fm_fallback.enabled = false;
    profile.analog_fm_fallback.rx = {0, 500, 25000, "RX2"};
    profile.analog_fm_fallback.fm = {2500, 3000, -740};
    profile.analog_fm_fallback.ctcss = {true, 1230, 120, 250, 200};
    profile.analog_fm_fallback.dmr_idle_guard_ms = 0;
    profile.analog_fm_fallback.dmr_tx = {
        1, 9999, 0xFFFFFFU, 1, CallType::AllCall};
    return profile;
}

ChannelProfile make_lab_vhf_to_uhf_profile()
{
    ChannelProfile profile;
    profile.id = "lab-vhf-to-uhf-828s";
    profile.dmr_rx = {1, 145400000, 100000, 250, 200000, "RX2"};
    profile.dmr_tx = {0, 438500000, 100000, 550, 200000, "TX/RX"};
    profile.analog_fm_fallback.enabled = true;
    profile.analog_fm_fallback.rx = {0, 250, 25000, "RX2"};
    profile.analog_fm_fallback.fm = {2500, 3000, -740};
    profile.analog_fm_fallback.ctcss = {true, 1230, 120, 250, 200};
    profile.analog_fm_fallback.dmr_idle_guard_ms = 0;
    profile.analog_fm_fallback.dmr_tx = {
        1, 9999, 0xFFFFFFU, 1, CallType::AllCall};
    return profile;
}

} // namespace dmr_rpt
