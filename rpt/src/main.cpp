// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/audit.h"
#include "dmr_rpt/build_info.h"
#include "dmr_rpt/config.h"
#include "dmr_rpt/dmr_burst.h"
#include "dmr_rpt/event.h"
#include "dmr_rpt/network_protocol.h"
#include "dmr_rpt/rf_session.h"
#include "dmr_rpt/vector_manifest.h"
#if defined(DMR_B210_HAVE_HARDWARE_RUNTIME)
#include "dmr_rpt/hardware_runtime.h"
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef DMR_B210_DEFAULT_CONFIG_PATH
#define DMR_B210_DEFAULT_CONFIG_PATH "/etc/dmr-rpt/repeater.yaml"
#endif

namespace {

struct Options {
    std::filesystem::path config_path = DMR_B210_DEFAULT_CONFIG_PATH;
    std::optional<std::string> uhd_device;
    std::optional<std::int64_t> rx_frequency_hz;
    std::optional<std::int64_t> tx_frequency_hz;
    bool disable_fm = false;
};

struct PendingNetworkCommand {
    dmr_rpt::NetworkControlCommand command;
};

struct CalibrationSession {
    std::string id;
    int rx_channel = -1;
    dmr_rpt::RxCalibrationBand band = dmr_rpt::RxCalibrationBand::Low;
    std::int32_t gain_tenths_db = 0;
    std::int32_t previous_gain_tenths_db = 0;
    bool forwarding_was_enabled = false;
    dmr_rpt::RxSignalCalibrationCurve curve;
    std::size_t next_input_index = 0;
};

class NetworkCommandMailbox {
public:
    void push(dmr_rpt::NetworkControlCommand command)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back({std::move(command)});
    }

    std::vector<PendingNetworkCommand> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<PendingNetworkCommand> result;
        while (!commands_.empty()) {
            result.push_back(std::move(commands_.front()));
            commands_.pop_front();
        }
        return result;
    }

private:
    std::mutex mutex_;
    std::deque<PendingNetworkCommand> commands_;
};

struct NetworkRuntimeState {
    mutable std::mutex mutex;
    dmr_rpt::ValidatedConfig validated;
    bool forwarding_enabled = true;
    bool rf_running = false;
    bool rf_fault = false;
    std::string gain_selection_mode = "manual";
    std::string last_error;
    std::string calibration_state_json = "{\"state\":\"idle\"}";
    std::uint64_t recording_storage_bytes = 0;
};

volatile std::sig_atomic_t g_stop_requested = 0;
constexpr std::chrono::seconds kRuntimeStatusInterval{10};
constexpr std::uint64_t kRecordingStorageLimitBytes = 1000ULL * 1000ULL * 1000ULL;

std::uint64_t recording_directory_size(const std::filesystem::path& directory)
{
    std::uint64_t total = 0;
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) {
        return 0;
    }
    for (std::filesystem::recursive_directory_iterator iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, error);
         !error && iterator != std::filesystem::recursive_directory_iterator();
         iterator.increment(error)) {
        if (iterator->is_regular_file(error)) {
            const auto size = iterator->file_size(error);
            if (!error && size <= std::numeric_limits<std::uint64_t>::max() - total) {
                total += size;
            }
        }
        error.clear();
    }
    return total;
}

void handle_signal(int)
{
    g_stop_requested = 1;
}

void set_default_runtime_log_levels()
{
#if defined(_WIN32)
    _putenv_s("UHD_LOG_LEVEL", "warning");
#else
    setenv("UHD_LOG_LEVEL", "warning", 0);
#endif
}

void print_console(const std::string& body, bool error = false)
{
    std::ostream& output = error ? std::cerr : std::cout;
    dmr_rpt::write_console_message(output, body);
}

std::string format_frequency_mhz(std::int64_t frequency_hz)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(6)
        << static_cast<double>(frequency_hz) / 1000000.0;
    std::string value = out.str();
    while (!value.empty() && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.pop_back();
    return value;
}

std::string json_escape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string utc_now_string()
{
    const std::time_t now = std::time(nullptr);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &now);
#else
    gmtime_r(&now, &value);
#endif
    std::ostringstream out;
    out << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string calibration_state_json(const CalibrationSession* session,
                                   const std::string& state,
                                   const std::string& message = {})
{
    std::ostringstream out;
    out << "{\"state\":\"" << json_escape(state) << "\"";
    if (session) {
        const auto& required =
            dmr_rpt::rx_calibration_required_inputs(session->band);
        const std::size_t count = session->curve.points.size();
        out << ",\"session_id\":\"" << json_escape(session->id)
            << "\",\"rx_channel\":" << session->rx_channel
            << ",\"band\":\"" << dmr_rpt::to_string(session->band)
            << "\",\"range\":\"" << dmr_rpt::to_string(session->band)
            << "\",\"rx_gain_tenths_db\":" << session->gain_tenths_db
            << ",\"completed_points\":" << count;
        if (session->next_input_index < required.size()) {
            out << ",\"next_input_dbm\":" << required[session->next_input_index];
        } else {
            out << ",\"next_input_dbm\":null";
        }
        out << ",\"points\":[";
        for (std::size_t index = 0; index < count; ++index) {
            if (index) out << ',';
            const auto& point = session->curve.points[index];
            out << "{\"input_dbm\":" << point.input_dbm
                << ",\"measured_dbfs\":" << point.measured_dbfs
                << ",\"snr_db\":" << point.snr_db
                << ",\"signal_valid\":true}";
        }
        out << ']';
    }
    if (!message.empty()) {
        out << ",\"message\":\"" << json_escape(message) << '"';
    }
    out << '}';
    return out.str();
}

std::string calibration_curves_json(const dmr_rpt::RxSignalCalibrationConfig& config)
{
    std::ostringstream out;
    out << '[';
    bool first_curve = true;
    for (int channel = 0; channel < 2; ++channel) {
        const std::size_t index = static_cast<std::size_t>(channel);
        for (const auto band : {dmr_rpt::RxCalibrationBand::Low,
                                dmr_rpt::RxCalibrationBand::Medium,
                                dmr_rpt::RxCalibrationBand::High}) {
            const auto& curve = band == dmr_rpt::RxCalibrationBand::Low
                ? config.low[index]
                : band == dmr_rpt::RxCalibrationBand::Medium
                    ? config.medium[index] : config.high[index];
            if (!first_curve) out << ',';
            first_curve = false;
            out << "{\"rx_channel\":" << channel
                << ",\"band\":\"" << dmr_rpt::to_string(band) << "\""
                << ",\"range\":\"" << dmr_rpt::to_string(band) << "\""
                << ",\"rx_gain_tenths_db\":";
            if (curve.rx_gain_tenths_db) out << *curve.rx_gain_tenths_db;
            else out << "null";
            out << ",\"points\":[";
            for (std::size_t point_index = 0; point_index < curve.points.size(); ++point_index) {
                if (point_index) out << ',';
                const auto& point = curve.points[point_index];
                out << "{\"input_dbm\":" << point.input_dbm
                    << ",\"measured_dbfs\":" << point.measured_dbfs
                    << ",\"snr_db\":" << point.snr_db << '}';
            }
            out << "]}";
        }
    }
    out << ']';
    return out.str();
}

std::string channel_profile_json(const dmr_rpt::ChannelProfile& profile)
{
    const auto& fm = profile.analog_fm_fallback;
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(profile.id)
        << "\",\"dmr_rx\":{\"channel\":" << profile.dmr_rx.channel
        << ",\"frequency_hz\":" << profile.dmr_rx.frequency_hz
        << ",\"lo_offset_hz\":" << profile.dmr_rx.lo_offset_hz
        << ",\"gain_tenths_db\":" << profile.dmr_rx.gain_tenths_db
        << ",\"bandwidth_hz\":" << profile.dmr_rx.bandwidth_hz
        << ",\"antenna\":\"" << json_escape(profile.dmr_rx.antenna)
        << "\"},\"dmr_tx\":{\"channel\":" << profile.dmr_tx.channel
        << ",\"frequency_hz\":" << profile.dmr_tx.frequency_hz
        << ",\"lo_offset_hz\":" << profile.dmr_tx.lo_offset_hz
        << ",\"gain_tenths_db\":" << profile.dmr_tx.gain_tenths_db
        << ",\"bandwidth_hz\":" << profile.dmr_tx.bandwidth_hz
        << ",\"antenna\":\"" << json_escape(profile.dmr_tx.antenna)
        << "\"},\"fm\":{\"enabled\":" << (fm.enabled ? "true" : "false")
        << ",\"ctcss_tone_tenths_hz\":"
        << fm.ctcss.tone_tenths_hz
        << ",\"squelch_tenths_dbfs\":"
        << fm.fm.squelch_tenths_dbfs << "}}";
    return out.str();
}

std::string active_receive_gain_mode(const dmr_rpt::RepeaterConfig& config)
{
    const auto profile = std::find_if(
        config.channel_profiles.begin(), config.channel_profiles.end(),
        [&](const dmr_rpt::ChannelProfile& item) {
            return item.id == config.radio.active_channel_profile_id;
        });
    if (profile == config.channel_profiles.end()) {
        return "custom";
    }
    const auto& gain_control = config.radio.receive_gain_control;
    if (profile->dmr_rx.gain_tenths_db ==
        gain_control.high_gain_tenths_db) {
        return "high";
    }
    if (profile->dmr_rx.gain_tenths_db ==
        gain_control.low_gain_tenths_db) {
        return "low";
    }
    return "custom";
}

void apply_active_receive_gain_mode(dmr_rpt::RepeaterConfig& config,
                                   const std::string& mode)
{
    const std::int32_t gain_tenths_db =
        dmr_rpt::receive_gain_tenths_db_for_mode(
            config.radio.receive_gain_control, mode);
    const auto profile = std::find_if(
        config.channel_profiles.begin(), config.channel_profiles.end(),
        [&](const dmr_rpt::ChannelProfile& item) {
            return item.id == config.radio.active_channel_profile_id;
        });
    if (profile == config.channel_profiles.end()) {
        throw std::runtime_error("active channel profile not found for gain control");
    }
    profile->dmr_rx.gain_tenths_db = gain_tenths_db;
    profile->analog_fm_fallback.rx.gain_tenths_db = gain_tenths_db;
}

std::string gain_control_json(const dmr_rpt::RepeaterConfig& config,
                              const std::string& selection_mode)
{
    const auto profile = std::find_if(
        config.channel_profiles.begin(), config.channel_profiles.end(),
        [&](const dmr_rpt::ChannelProfile& item) {
            return item.id == config.radio.active_channel_profile_id;
        });
    const auto& gain_control = config.radio.receive_gain_control;
    std::ostringstream out;
    const std::string active_mode = active_receive_gain_mode(config);
    out << "{\"mode\":\"" << active_mode
        << "\",\"selection_mode\":\"" << selection_mode
        << "\",\"active_mode\":\"" << active_mode
        << "\",\"high_gain_tenths_db\":"
        << gain_control.high_gain_tenths_db
        << ",\"low_gain_tenths_db\":"
        << gain_control.low_gain_tenths_db;
    if (profile != config.channel_profiles.end()) {
        out << ",\"active_dmr_rx_gain_tenths_db\":"
            << profile->dmr_rx.gain_tenths_db
            << ",\"active_fm_rx_gain_tenths_db\":"
            << profile->analog_fm_fallback.rx.gain_tenths_db;
    }
    const auto& automatic = gain_control.automatic_switching;
    out << ",\"automatic_switching\":{\"enabled\":"
        << (automatic.enabled ? "true" : "false")
        << ",\"high_to_low_threshold_dbm\":"
        << automatic.high_to_low_threshold_dbm
        << ",\"low_to_high_threshold_dbm\":"
        << automatic.low_to_high_threshold_dbm << '}';
    out << '}';
    return out.str();
}

std::string runtime_status_json(const NetworkRuntimeState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto& validated = state.validated;
    std::ostringstream out;
    out << "{\"v\":1,\"type\":\"status\",\"active_channel_profile_id\":\""
        << json_escape(validated.rf.active_channel_profile_id)
        << "\",\"configured_channel_profile_count\":"
        << validated.rf.configured_channel_profile_count
        << ",\"forwarding_enabled\":"
        << (state.forwarding_enabled ? "true" : "false")
        << ",\"rf_running\":"
        << (state.rf_running ? "true" : "false")
        << ",\"rf_fault\":"
        << (state.rf_fault ? "true" : "false")
        << ",\"gain_control\":"
        << gain_control_json(validated.config, state.gain_selection_mode)
        << ",\"recording_storage_bytes\":"
        << state.recording_storage_bytes
        << ",\"recording_storage_limit_bytes\":"
        << kRecordingStorageLimitBytes
        << ",\"last_error\":\""
        << json_escape(state.last_error) << "\"}";
    return out.str();
}

std::string version_status_json()
{
    std::ostringstream out;
    out << "{\"repeater_version\":\"V" << DMR_B210_RELEASE_VERSION
        << "\",\"build_sequence\":" << DMR_B210_BUILD_SEQUENCE << '}';
    return out.str();
}

std::string profile_status_json(const NetworkRuntimeState& state,
                                const std::string& requested)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    const std::string profile_id = requested.empty()
        ? state.validated.rf.active_channel_profile_id
        : requested;
    const auto found = std::find_if(
        state.validated.config.channel_profiles.begin(),
        state.validated.config.channel_profiles.end(),
        [&](const dmr_rpt::ChannelProfile& profile) {
            return profile.id == profile_id;
        });
    if (found == state.validated.config.channel_profiles.end()) {
        return "{}";
    }
    return channel_profile_json(*found);
}

bool has_channel_patch(const dmr_rpt::NetworkChannelPatch& patch)
{
    return patch.rx_frequency_hz || patch.tx_frequency_hz ||
        patch.rx_gain_tenths_db || patch.tx_gain_tenths_db ||
        patch.fm_enabled || patch.ctcss_tone_tenths_hz;
}

void print_console_path(const std::string& label,
                        const std::filesystem::path& path)
{
    constexpr std::size_t chunk_size = 16U;
    const std::string value = path.string();
    std::size_t part = 1U;
    for (std::size_t offset = 0; offset < value.size(); offset += chunk_size) {
        print_console(label + std::to_string(part++) + ' ' +
                      value.substr(offset, chunk_size));
    }
}

void print_usage(const char* program, bool detailed)
{
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Normal startup uses " << DMR_B210_DEFAULT_CONFIG_PATH
        << " and requires no command-line parameters.\n"
        << "All overrides apply only to this process and do not rewrite YAML.\n\n"
        << "Startup options:\n"
        << "  --config <repeater.yaml>  Select the YAML configuration file\n"
        << "  --uhd-device <args>       Pass UHD driver --args parameters\n"
        << "  --args <args>             Alias for --uhd-device\n"
        << "  --rx-frequency <Hz>       Override active-profile DMR RX frequency\n"
        << "  --tx-frequency <Hz>       Override active-profile DMR TX frequency\n"
        << "  --disable-fm               Disable analog FM receive and relay\n"
        << "\nInformation options:\n"
        << "  --help, -h                 Show this help\n"
        << "  --help-detail              Show full option and abbreviation meanings\n"
        << "  --version                  Show release version and build sequence\n";
    if (!detailed) {
        return;
    }

    std::cout
        << "\nDetailed option behavior:\n"
        << "  --uhd-device and --args are exact aliases. Their value is passed\n"
        << "  unchanged to the UHD device-address parser used by the B210.\n"
        << "  The value must describe a B210 and include type=b200.\n"
        << "  --rx-frequency and --tx-frequency accept whole Hz values and\n"
        << "  override only the active profile. Both remain independently checked.\n"
        << "  --disable-fm suppresses analog FM processing for every saved profile.\n"
        << "\nAbbreviations:\n"
        << "  UHD = USRP Hardware Driver, the radio device driver and address format.\n"
        << "  USRP = Universal Software Radio Peripheral, the B210 radio family.\n"
        << "  DMR = Digital Mobile Radio.\n"
        << "  RX = Receive, the radio receive path.\n"
        << "  TX = Transmit, the radio transmit path.\n"
        << "  FM = Frequency Modulation, analog radio modulation.\n"
        << "  Hz = Hertz, the frequency unit.\n"
        << "  YAML = YAML Ain't Markup Language, the configuration file format.\n";
}

bool has_direct_lab_manifest(const std::filesystem::path& vector_root)
{
    std::error_code error;
    return std::filesystem::is_regular_file(
        vector_root / "direct_voice_group_slot1_001" / "manifest.json", error);
}

std::filesystem::path resolve_default_vector_root(const char* program)
{
    std::error_code error;
    std::filesystem::path executable =
        std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        error.clear();
        executable = std::filesystem::absolute(program, error);
    }

#if defined(DMR_B210_INSTALLED_VECTOR_RELATIVE_PATH)
    if (!error) {
        const std::filesystem::path installed =
            (executable.parent_path() /
             DMR_B210_INSTALLED_VECTOR_RELATIVE_PATH).lexically_normal();
        if (has_direct_lab_manifest(installed)) {
            return installed;
        }
    }
#endif

    return std::filesystem::path("test-vectors/828s");
}

std::int64_t parse_frequency_hz_option(const std::string& value,
                                       const char* flag)
{
    std::size_t consumed = 0;
    const long long parsed = std::stoll(value, &consumed, 10);
    if (consumed != value.size() || parsed <= 0) {
        throw std::runtime_error(std::string(flag) +
                                 " must be a positive integer number of Hz");
    }
    return static_cast<std::int64_t>(parsed);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--help" || flag == "-h") {
            print_usage(argv[0], false);
            std::exit(0);
        }
        if (flag == "--help-detail") {
            print_usage(argv[0], true);
            std::exit(0);
        }
        if (flag == "--version") {
            print_console(std::string("V") + DMR_B210_RELEASE_VERSION +
                          " B" + std::to_string(DMR_B210_BUILD_SEQUENCE));
            std::exit(0);
        }
        if (flag == "--disable-fm") {
            options.disable_fm = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + flag);
        }
        const std::string value = argv[++index];
        if (flag == "--config") {
            options.config_path = value;
        } else if (flag == "--uhd-device" || flag == "--args") {
            options.uhd_device = value;
        } else if (flag == "--rx-frequency") {
            options.rx_frequency_hz =
                parse_frequency_hz_option(value, "--rx-frequency");
        } else if (flag == "--tx-frequency") {
            options.tx_frequency_hz =
                parse_frequency_hz_option(value, "--tx-frequency");
        } else {
            throw std::runtime_error("unknown option: " + flag);
        }
    }
    return options;
}

void apply_startup_overrides(dmr_rpt::RepeaterConfig& config,
                             const Options& options)
{
    if (options.uhd_device) {
        config.radio.uhd_device = *options.uhd_device;
    }
    for (dmr_rpt::ChannelProfile& profile : config.channel_profiles) {
        if (options.disable_fm) {
            profile.analog_fm_fallback.enabled = false;
        }
        if (profile.id != config.radio.active_channel_profile_id) {
            continue;
        }
        if (options.rx_frequency_hz) {
            profile.dmr_rx.frequency_hz = *options.rx_frequency_hz;
        }
        if (options.tx_frequency_hz) {
            profile.dmr_tx.frequency_hz = *options.tx_frequency_hz;
        }
    }
}

std::int64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool verify_direct_lab_vectors(const std::filesystem::path& vector_root,
                               std::string& error)
{
    const std::array<std::pair<const char*, const char*>, 1> required{{
        {"direct_voice_group_slot1_001", "voice"},
    }};
    for (const auto& [vector_id, air_case_token] : required) {
        const auto manifest = vector_root / vector_id / "manifest.json";
        if (!std::filesystem::exists(manifest)) {
            error = "direct_lab manifest is missing at " + manifest.string();
            return false;
        }
        const dmr_rpt::ManifestVerification verification =
            dmr_rpt::verify_vector_manifest(manifest, false);
        if (!verification.ok) {
            error = verification.errors.empty()
                ? "direct_lab manifest verification failed"
                : verification.errors.front();
            return false;
        }
        if (verification.manifest.profile != "direct_lab" ||
            verification.manifest.vector_id != vector_id ||
            verification.manifest.air_case.find(air_case_token) ==
                std::string::npos) {
            error = std::string(vector_id) +
                " declares an incompatible profile or air case";
            return false;
        }
    }
    return true;
}

void print_configuration(const dmr_rpt::ValidatedConfig& validated,
                         const std::string& profile_gate,
                         const std::filesystem::path& vector_root,
                         bool fm_disabled_by_cli)
{
    const auto& profile = validated.rf.active_profile;
    print_console("CFG OK HW " + validated.rf.active_channel_profile_id);
    print_console("RX" + format_frequency_mhz(profile.dmr_rx.frequency_hz) +
                  " TX" + format_frequency_mhz(profile.dmr_tx.frequency_hz));
    print_console(std::string("FM") +
                  (profile.analog_fm_fallback.enabled ? "1" : "0") +
                  std::string(" ") + dmr_rpt::console_token::Ctcss +
                  std::to_string(
                      profile.analog_fm_fallback.ctcss.tone_tenths_hz / 10));
    print_console(std::string("VEC ") + profile_gate +
                  (fm_disabled_by_cli
                       ? std::string(" ") + dmr_rpt::console_token::DisableFm
                       : ""));
    print_console_path("V", vector_root);
}

void print_runtime_status(const dmr_rpt::ValidatedConfig& validated,
                          std::int64_t uptime_seconds)
{
    const bool analog_fm_enabled =
        validated.rf.active_profile.analog_fm_fallback.enabled;
    std::ostringstream line;
    line << "ST U" << uptime_seconds << " FM" << (analog_fm_enabled ? '1' : '0');
    if (!analog_fm_enabled) {
        line << " S1 P"
             << dmr_rpt::kSingleStreamTxStartupPrefillFrames *
                    dmr_rpt::kDirectModeFrameDurationMs;
    }
    print_console(line.str());
}

int run_session(dmr_rpt::B210SessionFactory& factory,
                dmr_rpt::ValidatedConfig& validated,
                dmr_rpt::OperationAuditLogger& audit,
                const std::filesystem::path& config_path,
                const std::filesystem::path& vector_root,
                const std::string& profile_gate,
                bool fm_disabled_by_cli,
                const std::shared_ptr<dmr_rpt::NetworkControlService>& network,
                const std::shared_ptr<NetworkCommandMailbox>& mailbox,
                const std::shared_ptr<std::atomic_bool>& forwarding_enabled,
                const std::shared_ptr<NetworkRuntimeState>& state,
                const std::shared_ptr<dmr_rpt::RxSignalCalibrationRuntime>& calibration)
{
    dmr_rpt::RfReinitializationController rf(factory);
    dmr_rpt::ValidatedConfig active_validated = validated;
    std::optional<CalibrationSession> calibration_session;
    const dmr_rpt::RfReinitializationResult started =
        rf.start_initial(active_validated.rf);
    if (!started.activated) {
        audit.emit({"RF", "rf_session.failed", "start", "failed",
                    validated.semantic_sha256, {{"error", started.error}}});
        forwarding_enabled->store(false);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->forwarding_enabled = false;
            state->rf_running = false;
            state->rf_fault = true;
            state->last_error = started.error;
        }
        audit.emit({"RF", "rf_fault.latched", "initial_start", "failed",
                    validated.semantic_sha256, {{"error", started.error}}});
        print_console("ERR RF FAULT", true);
    }
    if (started.activated) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->rf_running = true;
        state->rf_fault = false;
        state->last_error.clear();
    }

    print_configuration(validated, profile_gate, vector_root,
                        fm_disabled_by_cli);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    const auto runtime_started_at = std::chrono::steady_clock::now();
    auto next_status_at = runtime_started_at + kRuntimeStatusInterval;
    auto next_rf_health_check_at = runtime_started_at + std::chrono::seconds(1);
    print_runtime_status(validated, 0);
    while (!g_stop_requested) {
        for (const PendingNetworkCommand& pending : mailbox->take()) {
            const auto& command = pending.command;
            if (command.operation == "stop_forwarding") {
                // The UDP handler clears this first to block new relays.  The
                // RF owner must also stop and destroy the B210 stream graph.
                forwarding_enabled->store(false);
                rf.stop();
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->forwarding_enabled = false;
                    state->rf_running = false;
                    state->rf_fault = false;
                    state->last_error.clear();
                }
                audit.emit({"NET", "forwarding.stopped", "stop_forwarding", "ok",
                            validated.semantic_sha256, {}});
                print_console("RF OFF");
                continue;
            }
            if (command.operation == "start_forwarding") {
                forwarding_enabled->store(false);
                if (!rf.running()) {
                    const dmr_rpt::RfReinitializationResult result =
                        rf.start_initial(validated.rf);
                    if (!result.activated) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->forwarding_enabled = false;
                        state->rf_running = false;
                        state->rf_fault = true;
                        state->last_error = result.error;
                        audit.emit({"NET", "forwarding.started", "start_forwarding",
                                    "failed", validated.semantic_sha256,
                                    {{"error", result.error}}});
                        print_console("ERR RF ON", true);
                        continue;
                    }
                    active_validated = validated;
                }
                forwarding_enabled->store(true);
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->forwarding_enabled = true;
                    state->rf_running = true;
                    state->rf_fault = false;
                    state->last_error.clear();
                }
                audit.emit({"NET", "forwarding.started", "start_forwarding", "ok",
                            validated.semantic_sha256, {}});
                print_console("RF ON");
                continue;
            }
            if (command.operation == "rx_calibration_begin" ||
                command.operation == "rx_calibration_step" ||
                command.operation == "rx_calibration_commit" ||
                command.operation == "rx_calibration_cancel") {
                auto set_calibration_state = [&](const std::string& value) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->calibration_state_json = value;
                };
                auto restore_forwarding = [&] {
                    if (!calibration_session) return;
                    std::string ignored_gain_error;
                    rf.set_rx_gain(calibration_session->rx_channel,
                                   calibration_session->previous_gain_tenths_db,
                                   ignored_gain_error);
                    std::string ignored_agc_error;
                    rf.set_rx_hardware_agc(calibration_session->rx_channel,
                                           true, ignored_agc_error);
                    forwarding_enabled->store(
                        calibration_session->forwarding_was_enabled);
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->forwarding_enabled =
                        calibration_session->forwarding_was_enabled;
                };
                if (command.operation == "rx_calibration_begin") {
                    if (calibration_session) {
                        set_calibration_state(calibration_state_json(
                            calibration_session ? &*calibration_session : nullptr,
                            "busy", "calibration session already active"));
                        continue;
                    }
                    if (!rf.running() || !command.calibration_rx_channel ||
                        command.calibration_rx_channel.value() < 0 ||
                        command.calibration_rx_channel.value() > 1) {
                        set_calibration_state(calibration_state_json(
                            nullptr, "error", "RF is stopped or RX channel is invalid"));
                        continue;
                    }
                    const auto band = dmr_rpt::rx_calibration_band_from_string(
                        command.calibration_band);
                    if (!band) {
                        set_calibration_state(calibration_state_json(
                            nullptr, "error", "calibration_band must be low, medium or high"));
                        continue;
                    }
                    if (!command.calibration_rx_gain_tenths_db ||
                        *command.calibration_rx_gain_tenths_db < 0 ||
                        *command.calibration_rx_gain_tenths_db > 1000 ||
                        (*band == dmr_rpt::RxCalibrationBand::Low &&
                         *command.calibration_rx_gain_tenths_db != 0) ||
                        (*band != dmr_rpt::RxCalibrationBand::Low &&
                         *command.calibration_rx_gain_tenths_db <= 0)) {
                        set_calibration_state(calibration_state_json(
                            nullptr, "error",
                            "low calibration requires 0 dB; medium/high calibration requires positive RX gain"));
                        continue;
                    }
                    const int channel = *command.calibration_rx_channel;
                    const auto observation = rf.calibration_observation(channel);
                    if (!observation) {
                        set_calibration_state(calibration_state_json(
                            nullptr, "error", "RX telemetry is not ready"));
                        continue;
                    }
                    CalibrationSession next;
                    next.id = "rxcal-" + std::to_string(monotonic_ms());
                    next.rx_channel = channel;
                    next.band = *band;
                    next.previous_gain_tenths_db = observation->rx_gain_tenths_db;
                    next.gain_tenths_db = *command.calibration_rx_gain_tenths_db;
                    next.forwarding_was_enabled = forwarding_enabled->load();
                    std::string gain_error;
                    if (!rf.set_rx_gain(channel, next.gain_tenths_db, gain_error)) {
                        set_calibration_state(calibration_state_json(
                            nullptr, "error", gain_error));
                        continue;
                    }
                    const auto& required = dmr_rpt::rx_calibration_required_inputs(*band);
                    next.curve = {};
                    next.curve.rx_gain_tenths_db = next.gain_tenths_db;
                    next.next_input_index = 0;
                    (void)required;
                    calibration_session = std::move(next);
                    calibration->clear_observations(channel);
                    forwarding_enabled->store(false);
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->forwarding_enabled = false;
                        state->calibration_state_json = calibration_state_json(
                            &*calibration_session, "active");
                        state->last_error.clear();
                    }
                    audit.emit({"CAL", "rx_calibration.started", "begin",
                                "ok", validated.semantic_sha256,
                                {{"session_id", calibration_session->id},
                                 {"rx_channel", std::to_string(channel)},
                                 {"band", dmr_rpt::to_string(*band)}}});
                    continue;
                }
                if (!calibration_session ||
                    command.calibration_session_id != calibration_session->id) {
                    set_calibration_state(calibration_state_json(
                        nullptr, "error", "invalid or expired calibration session"));
                    continue;
                }
                if (command.operation == "rx_calibration_cancel") {
                    restore_forwarding();
                    set_calibration_state(calibration_state_json(
                        nullptr, "cancelled"));
                    calibration_session.reset();
                    continue;
                }
                if (command.operation == "rx_calibration_step") {
                    const auto& required = dmr_rpt::rx_calibration_required_inputs(
                        calibration_session->band);
                    const std::size_t index = calibration_session->next_input_index;
                    if (index >= required.size() || !command.calibration_input_dbm ||
                        command.calibration_input_dbm.value() != required[index]) {
                        set_calibration_state(calibration_state_json(
                            &*calibration_session, "error",
                            "input level is out of order"));
                        continue;
                    }
                    std::string gain_error;
                    if (!rf.set_rx_gain(calibration_session->rx_channel,
                                        calibration_session->gain_tenths_db,
                                        gain_error)) {
                        set_calibration_state(calibration_state_json(
                            &*calibration_session, "error", gain_error));
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(650));
                    const auto observation = calibration->stable_observation(
                        calibration_session->rx_channel, 10U, monotonic_ms(),
                        2500, 0.8);
                    const double minimum_snr_db =
                        calibration_session->band ==
                            dmr_rpt::RxCalibrationBand::High
                        ? 12.0 : 10.0;
                    if (!observation || !observation->measured_dbfs ||
                        !observation->snr_db ||
                        *observation->snr_db < minimum_snr_db) {
                        set_calibration_state(calibration_state_json(
                            &*calibration_session, "signal_unstable",
                            "ten fresh samples within 0.8 dB and the range SNR threshold are required"));
                        continue;
                    }
                    calibration_session->curve.rx_gain_tenths_db =
                        calibration_session->gain_tenths_db;
                    calibration_session->curve.points.erase(
                        std::remove_if(calibration_session->curve.points.begin(),
                                       calibration_session->curve.points.end(),
                            [&](const auto& point) {
                                return point.input_dbm == *command.calibration_input_dbm;
                            }), calibration_session->curve.points.end());
                    calibration_session->curve.points.push_back({
                        *command.calibration_input_dbm,
                        *observation->measured_dbfs,
                        *observation->snr_db,
                        utc_now_string()});
                    ++calibration_session->next_input_index;
                    set_calibration_state(calibration_state_json(
                        &*calibration_session, "active"));
                    continue;
                }
                if (!dmr_rpt::rx_calibration_curve_complete(
                        calibration_session->curve, calibration_session->band)) {
                    set_calibration_state(calibration_state_json(
                        &*calibration_session, "error",
                        "all valid calibration points are required before commit"));
                    continue;
                }
                dmr_rpt::RepeaterConfig candidate = validated.config;
                const std::size_t calibration_channel = static_cast<std::size_t>(
                    calibration_session->rx_channel);
                if (calibration_session->band == dmr_rpt::RxCalibrationBand::Low) {
                    candidate.radio.rx_signal_calibration.low[calibration_channel] =
                        calibration_session->curve;
                } else if (calibration_session->band ==
                           dmr_rpt::RxCalibrationBand::Medium) {
                    candidate.radio.rx_signal_calibration.medium[calibration_channel] =
                        calibration_session->curve;
                } else {
                    candidate.radio.rx_signal_calibration.high[calibration_channel] =
                        calibration_session->curve;
                }
                try {
                    dmr_rpt::persist_rx_signal_calibration(
                        config_path, candidate.radio.rx_signal_calibration);
                    validated = dmr_rpt::validate_config(candidate);
                    active_validated = validated;
                    calibration->replace(candidate.radio.rx_signal_calibration);
                    restore_forwarding();
                    std::ostringstream committed_state;
                    committed_state << "{\"state\":\"committed\","
                        << "\"config_written\":true,\"rx_channel\":"
                        << calibration_session->rx_channel
                        << ",\"band\":\""
                        << dmr_rpt::to_string(calibration_session->band)
                        << "\",\"rx_gain_tenths_db\":"
                        << calibration_session->gain_tenths_db
                        << ",\"completed_points\":"
                        << calibration_session->curve.points.size() << '}';
                    set_calibration_state(committed_state.str());
                    audit.emit({"CAL", "rx_calibration.committed", "commit",
                                "ok", validated.semantic_sha256,
                                {{"rx_channel", std::to_string(
                                     calibration_session->rx_channel)},
                                 {"band", dmr_rpt::to_string(
                                     calibration_session->band)},
                                 {"config_written", "true"}}});
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->validated = validated;
                        state->last_error.clear();
                    }
                    calibration_session.reset();
                } catch (const std::exception& error) {
                    set_calibration_state(calibration_state_json(
                        &*calibration_session, "error", error.what()));
                }
                continue;
            }
            if (command.operation != "switch_channel" &&
                command.operation != "set_channel" &&
                command.operation != "save_channel" &&
                command.operation != "set_gain_mode") {
                continue;
            }
            if (!rf.running() && command.operation != "save_channel") {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_error =
                    "RF is stopped; start forwarding before changing configuration";
                continue;
            }
            dmr_rpt::RepeaterConfig candidate = validated.config;
            if (command.operation == "switch_channel") {
                if (command.profile_id.empty()) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->last_error = "profile_id is required";
                    continue;
                }
                candidate.radio.active_channel_profile_id =
                    command.profile_id;
            }
            const std::string profile_id = command.profile_id.empty()
                ? candidate.radio.active_channel_profile_id
                : command.profile_id;
            auto profile = std::find_if(
                candidate.channel_profiles.begin(),
                candidate.channel_profiles.end(),
                [&](const dmr_rpt::ChannelProfile& item) {
                    return item.id == profile_id;
                });
            if (profile == candidate.channel_profiles.end()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_error = "channel profile not found: " + profile_id;
                continue;
            }
            if (command.operation == "switch_channel") {
                const std::string previous_mode =
                    active_receive_gain_mode(validated.config);
                if (dmr_rpt::is_selectable_receive_gain_mode(previous_mode)) {
                    apply_active_receive_gain_mode(candidate, previous_mode);
                }
            }
            if (command.operation == "set_gain_mode") {
                if (command.gain_mode == "auto") {
                    candidate.radio.receive_gain_control.automatic_switching.enabled =
                        true;
                }
                const std::string requested_mode = command.gain_mode == "auto"
                    ? active_receive_gain_mode(validated.config)
                    : command.gain_mode;
                apply_active_receive_gain_mode(
                    candidate, dmr_rpt::is_selectable_receive_gain_mode(requested_mode)
                        ? requested_mode : "high");
            }
            const auto& patch = command.channel_patch;
            if (patch.rx_frequency_hz) {
                profile->dmr_rx.frequency_hz = *patch.rx_frequency_hz;
            }
            if (patch.tx_frequency_hz) {
                profile->dmr_tx.frequency_hz = *patch.tx_frequency_hz;
            }
            if (patch.rx_gain_tenths_db) {
                profile->dmr_rx.gain_tenths_db = *patch.rx_gain_tenths_db;
            }
            if (patch.tx_gain_tenths_db) {
                profile->dmr_tx.gain_tenths_db = *patch.tx_gain_tenths_db;
            }
            if (patch.fm_enabled) {
                profile->analog_fm_fallback.enabled = *patch.fm_enabled;
            }
            if (patch.ctcss_tone_tenths_hz) {
                profile->analog_fm_fallback.ctcss.tone_tenths_hz =
                    *patch.ctcss_tone_tenths_hz;
            }
            try {
                const dmr_rpt::ValidatedConfig candidate_validated =
                    dmr_rpt::validate_config(candidate);
                if (command.operation == "save_channel") {
                    dmr_rpt::persist_channel_profile(config_path, *profile);
                    validated = candidate_validated;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->validated = validated;
                        state->last_error.clear();
                    }
                    audit.emit({"NET", "channel_profile.saved", "save_channel", "ok",
                                validated.semantic_sha256,
                                {{"profile_id", profile_id},
                                 {"config_path", config_path.string()}}});
                    continue;
                }
                const dmr_rpt::RfReinitializationResult result =
                    rf.reinitialize(candidate_validated.rf, active_validated.rf);
                if (!result.activated) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->last_error = result.error;
                    continue;
                }
                const bool persist_active_profile =
                    command.operation == "switch_channel" &&
                    command.persist_active_profile.value_or(false);
                if (persist_active_profile) {
                    try {
                        dmr_rpt::persist_active_channel_profile(
                            config_path,
                            candidate_validated.rf.active_channel_profile_id);
                        audit.emit({"NET", "channel_profile.persisted",
                                    "switch_channel", "ok",
                                    candidate_validated.semantic_sha256,
                                    {{"active_profile",
                                      candidate_validated.rf
                                          .active_channel_profile_id},
                                     {"config_path", config_path.string()}}});
                    } catch (const std::exception& error) {
                        const dmr_rpt::RfReinitializationResult rollback =
                            rf.reinitialize(active_validated.rf, candidate_validated.rf);
                        std::string message =
                            "cannot persist active channel profile: " +
                            std::string(error.what());
                        if (!rollback.activated) {
                            message += "; runtime rollback failed: " +
                                rollback.error;
                        }
                        audit.emit({"NET", "channel_profile.persisted",
                                    "switch_channel", "failed",
                                    validated.semantic_sha256,
                                    {{"active_profile",
                                      candidate_validated.rf
                                          .active_channel_profile_id},
                                     {"config_path", config_path.string()},
                                     {"error", message}}});
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->last_error = message;
                        continue;
                    }
                }
                validated = candidate_validated;
                active_validated = candidate_validated;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->validated = validated;
                    if (command.operation == "set_gain_mode") {
                        state->gain_selection_mode = command.gain_mode == "auto"
                            ? "auto" : "manual";
                    } else if (command.operation == "set_channel" &&
                               command.channel_patch.rx_gain_tenths_db &&
                               profile_id == validated.config.radio
                                   .active_channel_profile_id) {
                        state->gain_selection_mode = "manual";
                    }
                    state->last_error.clear();
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_error = error.what();
            }
        }
        const std::int64_t poll_time_ms = monotonic_ms();
        rf.poll(poll_time_ms);
        const auto health_now = std::chrono::steady_clock::now();
        if (rf.running() && health_now >= next_rf_health_check_at) {
            std::string health_error;
            if (!rf.health_check(health_error)) {
                forwarding_enabled->store(false);
                rf.stop();
                const std::string fault = health_error.empty()
                    ? "B210 RF health check failed" : health_error;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->forwarding_enabled = false;
                    state->rf_running = false;
                    state->rf_fault = true;
                    state->last_error = fault;
                }
                audit.emit({"RF", "rf_fault.latched", "health_check", "failed",
                            validated.semantic_sha256, {{"error", fault}}});
                print_console("ERR RF FAULT", true);
            }
            do {
                next_rf_health_check_at += std::chrono::seconds(1);
            } while (next_rf_health_check_at <= health_now);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status_at) {
            const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - runtime_started_at).count();
            print_runtime_status(validated, uptime);
            do {
                next_status_at += kRuntimeStatusInterval;
            } while (next_status_at <= now);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    rf.stop();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->rf_running = false;
    }
    if (network) {
        network->stop();
    }
    audit.emit({"MAIN", "process.stopped", "run", "ok",
                validated.semantic_sha256, {}});
    print_console("ST OFF");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        std::cout << std::unitbuf;
        std::cerr << std::unitbuf;
        set_default_runtime_log_levels();
        const Options options = parse_options(argc, argv);
        print_console(std::string("V") + DMR_B210_RELEASE_VERSION +
                      " B" + std::to_string(DMR_B210_BUILD_SEQUENCE));
        const std::filesystem::path config_path =
            std::filesystem::absolute(options.config_path);
        dmr_rpt::RepeaterConfig config = dmr_rpt::load_config_file(config_path);
        apply_startup_overrides(config, options);
        std::string initial_gain_selection_mode = "manual";
        if (config.radio.receive_gain_control.default_mode == "auto") {
            apply_active_receive_gain_mode(config, "high");
            initial_gain_selection_mode = "auto";
        } else if (dmr_rpt::is_selectable_receive_gain_mode(
                       config.radio.receive_gain_control.default_mode)) {
            apply_active_receive_gain_mode(
                config, config.radio.receive_gain_control.default_mode);
        }
        dmr_rpt::ValidatedConfig validated =
            dmr_rpt::validate_config(config);
        const std::filesystem::path vector_root =
            resolve_default_vector_root(argv[0]);

        const std::filesystem::path config_directory = config_path.parent_path();
        if (validated.config.logging.recording_directory.is_relative()) {
            validated.config.logging.recording_directory =
                config_directory /
                validated.config.logging.recording_directory;
        }
        if (validated.config.local_audio.recording_directory.is_relative()) {
            validated.config.local_audio.recording_directory =
                config_directory /
                validated.config.local_audio.recording_directory;
        }
        if (validated.config.remote_voice.spool_directory.is_relative()) {
            validated.config.remote_voice.spool_directory =
                config_directory /
                validated.config.remote_voice.spool_directory;
        }

        dmr_rpt::LoggingConfig logging = validated.config.logging;
        if (logging.event_directory.is_relative()) {
            logging.event_directory = config_directory /
                logging.event_directory;
        }
        dmr_rpt::OperationAuditLogger audit(logging);
        print_console_path("LOG", std::filesystem::absolute(audit.path()));
        print_console_path("REC", std::filesystem::absolute(
                                   validated.config.logging.recording_directory));
        audit.emit({"MAIN", "process.config_validated", "load_config", "ok",
                    validated.semantic_sha256,
                    {{"active_profile", validated.rf.active_channel_profile_id},
                     {"fm_disabled_by_cli",
                       options.disable_fm ? "true" : "false"},
                     {"uhd_device_overridden",
                      options.uhd_device ? "true" : "false"},
                     {"rx_frequency_overridden",
                      options.rx_frequency_hz ? "true" : "false"},
                     {"tx_frequency_overridden",
                      options.tx_frequency_hz ? "true" : "false"},
                     {"configured_profiles",
                      std::to_string(validated.rf.configured_channel_profile_count)}}});

        if (validated.config.dmr.profile != dmr_rpt::DmrProfile::DirectLab) {
            audit.emit({"AIR", "profile.enablement_blocked", "start", "rejected",
                        validated.semantic_sha256,
                        {{"reason", "MissingVectorSet"},
                         {"profile", dmr_rpt::to_string(validated.config.dmr.profile)}}});
            print_console("ERR MissingVectorSet", true);
            return 3;
        }

        std::string vector_error;
        if (!verify_direct_lab_vectors(vector_root, vector_error)) {
            audit.emit({"AIR", "profile.enablement_blocked", "start", "rejected",
                        validated.semantic_sha256,
                        {{"reason", "MissingVectorSet"}, {"error", vector_error}}});
            print_console("ERR MissingVectorSet", true);
            return 3;
        }
        const bool full_vectors =
            dmr_rpt::t1_t2_data_vectors_available(vector_root);
        const std::string profile_gate = full_vectors ? "ALL" : "DL";
        auto forwarding_enabled = std::make_shared<std::atomic_bool>(true);
        auto calibration = std::make_shared<dmr_rpt::RxSignalCalibrationRuntime>(
            validated.config.radio.rx_signal_calibration);
        auto mailbox = std::make_shared<NetworkCommandMailbox>();
        auto state = std::make_shared<NetworkRuntimeState>();
        state->validated = validated;
        state->gain_selection_mode = initial_gain_selection_mode;
        state->recording_storage_bytes = recording_directory_size(
            validated.config.logging.recording_directory);
        const dmr_rpt::NetworkControlCallbacks callbacks{
            [state, mailbox, forwarding_enabled](
                const dmr_rpt::NetworkControlCommand& command) {
                dmr_rpt::NetworkControlResult result;
                if (command.operation == "get_status") {
                    result.accepted = true;
                    result.code = "ok";
                    result.state_json = runtime_status_json(*state);
                    return result;
                }
                if (command.operation == "get_version") {
                    result.accepted = true;
                    result.code = "ok";
                    result.state_json = version_status_json();
                    return result;
                }
                if (command.operation == "get_channel") {
                    result.state_json = profile_status_json(
                        *state, command.profile_id);
                    if (result.state_json == "{}") {
                        result.code = "not_found";
                        result.message = "channel profile not found";
                        return result;
                    }
                    result.accepted = true;
                    result.code = "ok";
                    return result;
                }
                if (command.operation == "get_gain_mode") {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    result.accepted = true;
                    result.code = "ok";
                    result.state_json = gain_control_json(
                        state->validated.config, state->gain_selection_mode);
                    return result;
                }
                if (command.operation == "get_rx_calibration") {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    result.accepted = true;
                    result.code = "ok";
                    result.state_json = state->calibration_state_json;
                    if (!result.state_json.empty() && result.state_json.back() == '}') {
                        result.state_json.pop_back();
                        result.state_json += ",\"curves\":" +
                            calibration_curves_json(
                                state->validated.config.radio.rx_signal_calibration) + '}';
                    }
                    return result;
                }
                if (command.operation == "start_forwarding" ||
                    command.operation == "stop_forwarding") {
                    if (command.operation == "stop_forwarding") {
                        // Block relay admission immediately; the RF owner will
                        // stop and release B210 in its main-loop turn.
                        forwarding_enabled->store(false);
                    }
                    mailbox->push(command);
                    result.accepted = true;
                    result.code = "accepted";
                    result.message = "queued for RF session activation";
                    return result;
                }
                if (command.operation == "switch_channel" ||
                    command.operation == "set_channel" ||
                    command.operation == "save_channel") {
                    if ((command.operation == "set_channel" ||
                         command.operation == "save_channel") &&
                        command.persist_active_profile.has_value()) {
                        result.code = "bad_request";
                        result.message =
                            "persist_active_profile is only valid for switch_channel";
                        return result;
                    }
                    if ((command.operation == "set_channel" ||
                         command.operation == "save_channel") &&
                        !has_channel_patch(command.channel_patch)) {
                        result.code = "bad_request";
                        result.message =
                            "channel update requires at least one channel field";
                        return result;
                    }
                    mailbox->push(command);
                    result.accepted = true;
                    result.code = "accepted";
                    result.message = command.operation == "save_channel"
                        ? "queued for configuration save"
                        : "queued for RF main-loop activation";
                    return result;
                }
                if (command.operation == "set_gain_mode") {
                    if (!command.profile_id.empty() ||
                        command.persist_active_profile.has_value() ||
                        has_channel_patch(command.channel_patch) ||
                        !dmr_rpt::is_receive_gain_selection_mode(
                            command.gain_mode)) {
                        result.code = "bad_request";
                        result.message =
                            "set_gain_mode requires gain_mode auto, high or low only";
                        return result;
                    }
                    mailbox->push(command);
                    result.accepted = true;
                    result.code = "accepted";
                    result.message = "queued for RF main-loop activation";
                    return result;
                }
                if (command.operation == "rx_calibration_begin" ||
                    command.operation == "rx_calibration_step" ||
                    command.operation == "rx_calibration_commit" ||
                    command.operation == "rx_calibration_cancel") {
                    const bool is_begin =
                        command.operation == "rx_calibration_begin";
                    if (is_begin && (!command.calibration_rx_channel ||
                                     !command.calibration_rx_gain_tenths_db ||
                                     !dmr_rpt::rx_calibration_band_from_string(
                                         command.calibration_band))) {
                        result.code = "bad_request";
                        result.message =
                            "rx_calibration_begin requires rx_channel, calibration_band and rx_gain_tenths_db";
                        return result;
                    }
                    if (!is_begin && command.calibration_session_id.empty()) {
                        result.code = "bad_request";
                        result.message = "calibration operation requires session_id";
                        return result;
                    }
                    if (command.operation == "rx_calibration_step" &&
                        !command.calibration_input_dbm) {
                        result.code = "bad_request";
                        result.message =
                            "rx_calibration_step requires input_dbm";
                        return result;
                    }
                    mailbox->push(command);
                    result.accepted = true;
                    result.code = "accepted";
                    result.message = "queued for calibration controller";
                    return result;
                }
                result.code = "bad_request";
                result.message = "unsupported network operation";
                return result;
            },
            [state] { return runtime_status_json(*state); },
            [state] {
                std::lock_guard<std::mutex> lock(state->mutex);
                return gain_control_json(state->validated.config,
                                         state->gain_selection_mode);
            }};
        auto network = std::make_shared<dmr_rpt::NetworkControlService>(
            validated.config.udp_control, validated.config.tcp_status,
            std::to_string(validated.config.dmr.repeater_id), callbacks);
        try {
            network->start();
        } catch (const std::exception& error) {
            audit.emit({"NET", "network.start_failed", "start", "failed",
                        validated.semantic_sha256,
                        {{"error", error.what()}}});
            print_console("ERR NET " + std::string(error.what()), true);
            return 5;
        }

#if defined(DMR_B210_HAVE_HARDWARE_RUNTIME)
        dmr_rpt::HardwareB210SessionFactory factory(
            validated, audit, false, network, forwarding_enabled, calibration,
            [state, recording_directory = validated.config.logging.recording_directory] {
                const auto bytes = recording_directory_size(recording_directory);
                std::lock_guard<std::mutex> lock(state->mutex);
                state->recording_storage_bytes = bytes;
            });
        return run_session(factory, validated, audit, config_path, vector_root,
                           profile_gate, options.disable_fm, network, mailbox,
                           forwarding_enabled, state, calibration);
#else
        network->stop();
        audit.emit({"RF", "runtime.unavailable", "start", "failed",
                    validated.semantic_sha256,
                    {{"reason", "HardwareRuntimeNotBuilt"}}});
        print_console("ERR NO HW", true);
        return 3;
#endif
    } catch (const std::exception& error) {
        print_console("ERR EXCEPTION " + std::string(error.what()), true);
        return 1;
    }
}
