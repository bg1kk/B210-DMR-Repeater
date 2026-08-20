// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include <SDL.h>
#include <SDL_ttf.h>
#include <yaml-cpp/yaml.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fb.h>
#include <linux/input.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr SDL_Color kBackground{16, 22, 26, 255};
constexpr SDL_Color kPanel{29, 39, 45, 255};
constexpr SDL_Color kLine{79, 96, 103, 255};
constexpr SDL_Color kText{232, 238, 239, 255};
constexpr SDL_Color kMuted{154, 171, 176, 255};
constexpr SDL_Color kCyan{0, 184, 196, 255};
constexpr SDL_Color kGreen{53, 187, 110, 255};
constexpr SDL_Color kRed{223, 74, 74, 255};
constexpr SDL_Color kAmber{235, 169, 58, 255};
constexpr SDL_Color kBlue{59, 142, 222, 255};
constexpr SDL_Color kDark{42, 53, 59, 255};

#ifndef DMR_B210_GUI_VERSION
#define DMR_B210_GUI_VERSION "unknown"
#endif

#include "dmr_rpt/build_info.h"

struct GuiConfig {
    std::filesystem::path config_path;
    std::string device_name = "DMR B210 转发器";
    std::string udp_address = "127.0.0.1";
    int udp_port = 42000;
    int listen_port = 43000;
    std::filesystem::path control_token_file;
    std::string control_token;
    std::string calibration_password = "14254328";
    int rotation_degrees = 0;
    int kmsdrm_device_index = -1;
    std::string framebuffer_path = "/dev/fb0";
    bool framebuffer_direct_output = false;
    std::string font_path =
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    std::string frequency_digit_font_path =
        "/opt/dmr-rpt/share/fonts/DashLCDSegment-Regular.ttf";
    std::array<double, 2> s9_reference_dbfs{-87.0, -87.0};
    std::vector<std::string> profile_ids;
    std::vector<std::string> quick_profile_ids;
};

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

int channel_sort_number(const std::string& profile_id)
{
    const std::string prefix = "channel-";
    if (profile_id.rfind(prefix, 0) != 0) {
        return std::numeric_limits<int>::max();
    }
    try {
        const std::size_t offset = prefix.size();
        if (offset == profile_id.size()) return std::numeric_limits<int>::max();
        const int number = std::stoi(profile_id.substr(offset));
        return number >= 0 ? number : std::numeric_limits<int>::max();
    } catch (...) {
        return std::numeric_limits<int>::max();
    }
}

void sort_channel_profiles(std::vector<std::string>& profile_ids)
{
    std::sort(profile_ids.begin(), profile_ids.end(), [](const std::string& left,
                                                         const std::string& right) {
        const int left_number = channel_sort_number(left);
        const int right_number = channel_sort_number(right);
        if (left_number != right_number) return left_number < right_number;
        return left < right;
    });
}

#if defined(__linux__)
std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    std::string value;
    std::getline(stream, value);
    return trim(value);
}

std::string select_dsi1_framebuffer(const std::string& fallback)
{
    try {
        const std::filesystem::path drm_root{"/sys/class/drm"};
        const std::filesystem::path graphics_root{"/sys/class/graphics"};
        for (const auto& connector : std::filesystem::directory_iterator(drm_root)) {
            const std::string name = connector.path().filename().string();
            if (name.find("-DSI-1") == std::string::npos ||
                read_text_file(connector.path() / "status") != "connected") {
                continue;
            }
            const auto dsi_device = std::filesystem::canonical(connector.path() / "device")
                                        .parent_path().parent_path();
            for (const auto& framebuffer : std::filesystem::directory_iterator(graphics_root)) {
                const std::string fb_name = framebuffer.path().filename().string();
                if (fb_name.rfind("fb", 0) != 0 || fb_name == "fbcon") continue;
                if (std::filesystem::canonical(framebuffer.path() / "device") == dsi_device) {
                    return "/dev/" + fb_name;
                }
            }
        }
    } catch (const std::filesystem::filesystem_error&) {
        // The configured path remains a safe fallback on nonstandard kernels.
    }
    return fallback;
}
#endif

GuiConfig load_config(const std::filesystem::path& path)
{
    const YAML::Node root = YAML::LoadFile(path.string());
    const YAML::Node gui = root["gui"] ? root["gui"] : root;
    GuiConfig config;
    config.config_path = path;
    if (gui["device_name"]) config.device_name = gui["device_name"].as<std::string>();
    if (gui["udp_address"]) config.udp_address = gui["udp_address"].as<std::string>();
    if (gui["udp_port"]) config.udp_port = gui["udp_port"].as<int>();
    if (gui["listen_port"]) config.listen_port = gui["listen_port"].as<int>();
    if (gui["control_token_file"]) {
        config.control_token_file = gui["control_token_file"].as<std::string>();
    }
    if (gui["control_token"]) config.control_token = gui["control_token"].as<std::string>();
    if (gui["calibration_password"]) {
        config.calibration_password = gui["calibration_password"].as<std::string>();
    }
    if (gui["display_rotation_degrees"]) {
        config.rotation_degrees = gui["display_rotation_degrees"].as<int>();
    }
    if (gui["kmsdrm_device_index"]) {
        config.kmsdrm_device_index = gui["kmsdrm_device_index"].as<int>();
    }
    if (gui["framebuffer_path"]) {
        config.framebuffer_path = gui["framebuffer_path"].as<std::string>();
    }
    if (gui["framebuffer_direct_output"]) {
        config.framebuffer_direct_output = gui["framebuffer_direct_output"].as<bool>();
    }
    if (gui["font_path"]) config.font_path = gui["font_path"].as<std::string>();
    if (gui["frequency_digit_font_path"]) {
        config.frequency_digit_font_path = gui["frequency_digit_font_path"].as<std::string>();
    }
    if (gui["s9_reference_dbfs"] && gui["s9_reference_dbfs"].IsSequence()) {
        for (std::size_t index = 0; index < 2U && index < gui["s9_reference_dbfs"].size(); ++index) {
            config.s9_reference_dbfs[index] = gui["s9_reference_dbfs"][index].as<double>();
        }
    }
    if (gui["profile_ids"] && gui["profile_ids"].IsSequence()) {
        for (const YAML::Node& id : gui["profile_ids"]) {
            config.profile_ids.push_back(id.as<std::string>());
        }
    }
    if (gui["quick_profile_ids"] && gui["quick_profile_ids"].IsSequence()) {
        for (const YAML::Node& id : gui["quick_profile_ids"]) {
            config.quick_profile_ids.push_back(id.as<std::string>());
        }
    }
    if (!config.control_token_file.empty()) {
        std::ifstream stream(config.control_token_file);
        if (!stream) {
            throw std::runtime_error("cannot read GUI control token file");
        }
        std::getline(stream, config.control_token);
        config.control_token = trim(config.control_token);
    }
    if (config.udp_port < 1 || config.udp_port > 65535 ||
        config.listen_port < 1 || config.listen_port > 65535 ||
        config.calibration_password.size() != 8U ||
        !std::all_of(config.calibration_password.begin(), config.calibration_password.end(),
                     [](unsigned char value) { return value >= '0' && value <= '9'; }) ||
        config.rotation_degrees % 90 != 0 || config.kmsdrm_device_index > 7) {
        throw std::runtime_error("invalid GUI network, calibration password or rotation configuration");
    }
    config.rotation_degrees = (config.rotation_degrees % 360 + 360) % 360;
    if (config.framebuffer_direct_output && config.rotation_degrees != 0) {
        throw std::runtime_error("direct framebuffer output requires zero display rotation");
    }
    const std::set<std::string> unique_quick_profiles(
        config.quick_profile_ids.begin(), config.quick_profile_ids.end());
    if (config.quick_profile_ids.size() > 3U ||
        unique_quick_profiles.size() != config.quick_profile_ids.size() ||
        std::any_of(config.quick_profile_ids.begin(), config.quick_profile_ids.end(),
                    [&](const std::string& profile_id) {
                        return std::find(config.profile_ids.begin(), config.profile_ids.end(),
                                         profile_id) == config.profile_ids.end();
                    })) {
        throw std::runtime_error("invalid GUI quick profile configuration");
    }
    sort_channel_profiles(config.quick_profile_ids);
    return config;
}

void persist_gui_quick_profiles(const std::filesystem::path& path,
                                const std::vector<std::string>& profile_ids)
{
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
        if (!root || !root.IsMap()) {
            throw std::runtime_error("GUI config root is not a map");
        }
        YAML::Node gui = root["gui"] ? root["gui"] : root;
        std::vector<std::string> sorted_profile_ids = profile_ids;
        sort_channel_profiles(sorted_profile_ids);
        YAML::Node quick_profiles(YAML::NodeType::Sequence);
        for (const std::string& profile_id : sorted_profile_ids) {
            quick_profiles.push_back(profile_id);
        }
        gui["quick_profile_ids"] = quick_profiles;
    } catch (const YAML::Exception& error) {
        throw std::runtime_error("cannot update GUI config: " +
                                 std::string(error.what()));
    }

    YAML::Emitter emitter;
    emitter.SetIndent(2);
    emitter << root;
    if (!emitter.good()) {
        throw std::runtime_error("cannot serialize GUI config");
    }

    const std::filesystem::path temporary_path =
        std::filesystem::path(path.string() + ".quick-profiles.tmp");
    const auto remove_temporary = [&temporary_path]() {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
    };
    std::error_code status_error;
    const std::filesystem::file_status original_status =
        std::filesystem::status(path, status_error);
    if (status_error) {
        throw std::runtime_error("cannot inspect GUI config permissions: " +
                                 status_error.message());
    }
    {
        std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create temporary GUI config");
        }
        output << emitter.c_str() << '\n';
        output.flush();
        if (!output) {
            output.close();
            remove_temporary();
            throw std::runtime_error("cannot write temporary GUI config");
        }
    }
    std::error_code permissions_error;
    std::filesystem::permissions(temporary_path, original_status.permissions(),
                                 std::filesystem::perm_options::replace,
                                 permissions_error);
    if (permissions_error) {
        remove_temporary();
        throw std::runtime_error("cannot preserve GUI config permissions: " +
                                 permissions_error.message());
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary_path, path, rename_error);
    if (rename_error) {
        remove_temporary();
        throw std::runtime_error("cannot replace GUI config: " +
                                 rename_error.message());
    }
}

std::string json_escape(const std::string& text)
{
    std::ostringstream out;
    for (const unsigned char character : text) {
        switch (character) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (character < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(character) << std::dec << std::setfill(' ');
            } else {
                out << static_cast<char>(character);
            }
        }
    }
    return out.str();
}

std::optional<std::size_t> json_value_position(const std::string& json,
                                               const std::string& key,
                                               std::size_t start = 0U)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t key_pos = json.find(needle, start);
    if (key_pos == std::string::npos) return std::nullopt;
    std::size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) ++pos;
    return pos < json.size() ? std::optional<std::size_t>(pos) : std::nullopt;
}

std::optional<std::string> json_string(const std::string& json,
                                       const std::string& key,
                                       std::size_t start = 0U)
{
    const auto pos = json_value_position(json, key, start);
    if (!pos || json[*pos] != '"') return std::nullopt;
    std::string value;
    bool escaped = false;
    for (std::size_t index = *pos + 1U; index < json.size(); ++index) {
        const char character = json[index];
        if (escaped) {
            switch (character) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(character); break;
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> json_number(const std::string& json, const std::string& key,
                             std::size_t start = 0U)
{
    const auto pos = json_value_position(json, key, start);
    if (!pos) return std::nullopt;
    try {
        std::size_t consumed = 0;
        if constexpr (std::is_integral_v<T>) {
            const long long value = std::stoll(json.substr(*pos), &consumed);
            return static_cast<T>(value);
        } else {
            const double value = std::stod(json.substr(*pos), &consumed);
            return static_cast<T>(value);
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> json_bool(const std::string& json, const std::string& key,
                              std::size_t start = 0U)
{
    const auto pos = json_value_position(json, key, start);
    if (!pos) return std::nullopt;
    if (json.compare(*pos, 4U, "true") == 0) return true;
    if (json.compare(*pos, 5U, "false") == 0) return false;
    return std::nullopt;
}

std::string json_object(const std::string& json, const std::string& key)
{
    const auto value = json_value_position(json, key);
    if (!value || json[*value] != '{') return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = *value; index < json.size(); ++index) {
        const char character = json[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') quoted = true;
        else if (character == '{') ++depth;
        else if (character == '}' && --depth == 0) return json.substr(*value, index - *value + 1U);
    }
    return {};
}

std::string json_array(const std::string& json, const std::string& key)
{
    const auto value = json_value_position(json, key);
    if (!value || json[*value] != '[') return {};
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = *value; index < json.size(); ++index) {
        const char character = json[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') quoted = true;
        else if (character == '[') ++depth;
        else if (character == ']' && --depth == 0) {
            return json.substr(*value, index - *value + 1U);
        }
    }
    return {};
}

std::vector<std::string> json_array_objects(const std::string& array)
{
    std::vector<std::string> objects;
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < array.size(); ++index) {
        const char character = array[index];
        if (quoted) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') quoted = false;
            continue;
        }
        if (character == '"') quoted = true;
        else if (character == '{') {
            if (depth++ == 0) begin = index;
        } else if (character == '}' && --depth == 0) {
            objects.push_back(array.substr(begin, index - begin + 1U));
        }
    }
    return objects;
}

std::string now_clock()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

std::string elapsed_clock(std::chrono::steady_clock::time_point started)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
        << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2) << seconds % 60;
    return out.str();
}

struct Receiver {
    bool receiving = false;
    bool rssi_valid = false;
    bool snr_valid = false;
    double rssi_dbfs = 0.0;
    bool rssi_dbm_valid = false;
    double rssi_dbm = 0.0;
    double snr_db = 0.0;
    std::string calibration_state = "uncalibrated";
    std::string mode = "未知";
    bool hardware_agc_enabled = false;
    bool analog_gain_valid = false;
    double analog_gain_db = 0.0;
    bool software_agc_gain_valid = false;
    double software_agc_gain_db = 0.0;
    bool agc_input_valid = false;
    double agc_input_dbfs = 0.0;
    bool rssi_gain_compensation_valid = false;
    double rssi_gain_compensation_db = 0.0;
};

struct Channel {
    std::string id;
    std::int64_t rx_frequency_hz = 0;
    std::int64_t tx_frequency_hz = 0;
    int rx_gain_tenths_db = 0;
    int tx_gain_tenths_db = 0;
    bool fm_enabled = false;
    int ctcss_tone_tenths_hz = 1230;
};

const std::array<int, 38> kStandardCtcssToneTenthsHz = {
    670, 719, 744, 770, 797, 825, 854, 885, 915, 948,
    974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273, 1318,
    1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862,
    1928, 2035, 2107, 2181, 2257, 2336, 2418, 2503
};

const std::array<int, 8> kLowCalibrationInputDbm = {
    -20, -25, -30, -35, -40, -45, -50, -55
};

const std::array<int, 9> kMediumCalibrationInputDbm = {
    -45, -50, -55, -60, -65, -70, -75, -80, -85
};

const std::array<int, 11> kHighCalibrationInputDbm = {
    -75, -80, -85, -90, -95, -100, -105, -110, -115, -120, -125
};

constexpr int kDefaultMediumCalibrationGainTenthsDb = 250;
constexpr int kDefaultHighCalibrationGainTenthsDb = 500;
constexpr double kCalibratedS1Dbm = -121.0;
constexpr double kCalibratedS9Dbm = kCalibratedS1Dbm + 48.0;

int standard_ctcss_tone(int tone_tenths_hz)
{
    const auto closest = std::min_element(
        kStandardCtcssToneTenthsHz.begin(), kStandardCtcssToneTenthsHz.end(),
        [tone_tenths_hz](int left, int right) {
            return std::abs(left - tone_tenths_hz) < std::abs(right - tone_tenths_hz);
        });
    return *closest;
}

int adjust_standard_ctcss_tone(int tone_tenths_hz, int direction)
{
    const int current = standard_ctcss_tone(tone_tenths_hz);
    const auto it = std::find(kStandardCtcssToneTenthsHz.begin(),
                              kStandardCtcssToneTenthsHz.end(), current);
    const auto index = static_cast<std::size_t>(std::distance(
        kStandardCtcssToneTenthsHz.begin(), it));
    if (direction < 0) {
        return index == 0 ? current : kStandardCtcssToneTenthsHz[index - 1];
    }
    return index + 1 >= kStandardCtcssToneTenthsHz.size()
        ? current
        : kStandardCtcssToneTenthsHz[index + 1];
}

struct ActiveCall {
    bool valid = false;
    int rx_channel = -1;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    std::string mode;
};

struct RuntimeState {
    bool online = false;
    bool forwarding_enabled = false;
    bool rf_running = false;
    bool rf_fault = false;
    bool stale = true;
    std::string active_profile;
    std::string repeater_version = "读取中";
    int repeater_build_sequence = 0;
    std::string working_mode = "idle";
    std::string gain_mode = "custom";
    std::string gain_selection_mode = "manual";
    int configured_profiles = 0;
    std::uint64_t recording_storage_bytes = 0;
    std::uint64_t recording_storage_limit_bytes = 1000000000ULL;
    std::string last_error;
    std::array<Receiver, 2> receivers{};
    ActiveCall active_call;
    Channel active_channel;
    std::uint64_t last_sequence = 0;
    std::chrono::steady_clock::time_point last_update{};
};

struct CalibrationUiState {
    int rx_channel = 0;
    std::string band = "low";
    std::string session_id;
    std::string state = "idle";
    std::optional<int> next_input_dbm;
    int completed_points = 0;
    int gain_tenths_db = 0;
    std::array<std::optional<int>, 6> column_gain{};
    std::array<std::map<int, std::pair<double, double>>, 6> column_points{};
    std::array<bool, 6> column_edited{};
    int selected_column = 0;
    bool authorized = false;
    std::string password_input;
    std::string password_error;
};

bool calibration_step_available(const CalibrationUiState& calibration)
{
    if (calibration.session_id.empty() || !calibration.next_input_dbm ||
        calibration.completed_points < 0) {
        return false;
    }
    if (calibration.band == "low") {
        return calibration.completed_points < static_cast<int>(kLowCalibrationInputDbm.size()) &&
            std::find(kLowCalibrationInputDbm.begin(), kLowCalibrationInputDbm.end(),
                      *calibration.next_input_dbm) != kLowCalibrationInputDbm.end();
    }
    if (calibration.band == "medium") {
        return calibration.completed_points < static_cast<int>(kMediumCalibrationInputDbm.size()) &&
            std::find(kMediumCalibrationInputDbm.begin(), kMediumCalibrationInputDbm.end(),
                      *calibration.next_input_dbm) != kMediumCalibrationInputDbm.end();
    }
    return calibration.completed_points < static_cast<int>(kHighCalibrationInputDbm.size()) &&
        std::find(kHighCalibrationInputDbm.begin(), kHighCalibrationInputDbm.end(),
                  *calibration.next_input_dbm) != kHighCalibrationInputDbm.end();
}

int calibration_band_index(const std::string& band)
{
    if (band == "medium") return 1;
    if (band == "high") return 2;
    return 0;
}

int calibration_required_count(const std::string& band)
{
    if (band == "medium") return static_cast<int>(kMediumCalibrationInputDbm.size());
    if (band == "high") return static_cast<int>(kHighCalibrationInputDbm.size());
    return static_cast<int>(kLowCalibrationInputDbm.size());
}

int selected_calibration_gain(const CalibrationUiState& calibration)
{
    if (calibration.band == "low") return 0;
    if (calibration.selected_column >= 0 && calibration.selected_column < 6) {
        const auto stored = calibration.column_gain[
            static_cast<std::size_t>(calibration.selected_column)];
        if (stored && *stored > 0) return *stored;
    }
    if (!calibration.session_id.empty() && calibration.gain_tenths_db > 0) {
        return calibration.gain_tenths_db;
    }
    return calibration.band == "medium"
        ? kDefaultMediumCalibrationGainTenthsDb
        : kDefaultHighCalibrationGainTenthsDb;
}

struct EventLine {
    std::string time;
    std::string message;
    SDL_Color color = kMuted;
};

class UdpClient {
public:
    explicit UdpClient(GuiConfig config) : config_(std::move(config))
    {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) throw std::runtime_error("cannot create GUI UDP socket");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            ::close(socket_);
            throw std::runtime_error("cannot bind GUI UDP listen port");
        }
        const int flags = fcntl(socket_, F_GETFL, 0);
        fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
        endpoint_.sin_family = AF_INET;
        endpoint_.sin_port = htons(static_cast<std::uint16_t>(config_.udp_port));
        if (::inet_pton(AF_INET, config_.udp_address.c_str(), &endpoint_.sin_addr) != 1) {
            ::close(socket_);
            throw std::runtime_error("GUI UDP address must be IPv4");
        }
    }

    ~UdpClient()
    {
        if (socket_ >= 0) ::close(socket_);
    }

    std::string send(const std::string& operation, const std::string& fields = {})
    {
        const std::string id = "gui-" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) +
            "-" + std::to_string(++request_counter_);
        std::ostringstream body;
        body << "{\"v\":1,\"id\":\"" << id << "\",\"op\":\""
             << json_escape(operation) << "\"";
        if (!fields.empty()) body << ',' << fields;
        if (!config_.control_token.empty()) {
            body << ",\"token\":\"" << json_escape(config_.control_token) << "\"";
        }
        body << '}';
        const std::string payload = body.str();
        const auto sent = ::sendto(socket_, payload.data(), payload.size(), 0,
            reinterpret_cast<const sockaddr*>(&endpoint_), sizeof(endpoint_));
        if (sent != static_cast<ssize_t>(payload.size())) {
            throw std::runtime_error("cannot send GUI UDP command");
        }
        return id;
    }

    std::vector<std::string> receive()
    {
        std::vector<std::string> received;
        char buffer[1400]{};
        while (true) {
            const ssize_t length = ::recvfrom(socket_, buffer, sizeof(buffer), 0, nullptr, nullptr);
            if (length <= 0) break;
            received.emplace_back(buffer, static_cast<std::size_t>(length));
        }
        return received;
    }

private:
    GuiConfig config_;
    int socket_ = -1;
    sockaddr_in endpoint_{};
    std::uint64_t request_counter_ = 0;
};

double hz_to_mhz(std::int64_t value)
{
    return static_cast<double>(value) / 1000000.0;
}

std::string frequency_text(std::int64_t value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << hz_to_mhz(value) << " MHz";
    return out.str();
}

std::string frequency_numeric_text(std::int64_t value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << hz_to_mhz(value);
    return out.str();
}

std::string gain_text(int value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << static_cast<double>(value) / 10.0 << " dB";
    return out.str();
}

class GuiApp {
public:
    GuiApp(GuiConfig config, bool stop_only)
        : config_(std::move(config)), client_(config_), stop_only_(stop_only)
    {
        if (!config_.quick_profile_ids.empty()) {
            quick_profile_ids_ = config_.quick_profile_ids;
        } else {
            for (const std::string& profile_id : config_.profile_ids) {
                if (quick_profile_ids_.size() >= 3U) break;
                quick_profile_ids_.push_back(profile_id);
            }
        }
        sort_channel_profiles(quick_profile_ids_);
    }

    int run()
    {
        if (stop_only_) return stop_forwarding();
        initialize_sdl();
        add_event("本地 UDP 已连接，正在读取状态", kAmber);
        send_initial_queries();
        const int result = event_loop();
        stop_status_subscription();
        shutdown_sdl();
        return result;
    }

    static bool self_test()
    {
        const auto level = s_meter(-87.0, -87.0);
        const auto started = std::chrono::steady_clock::now() - std::chrono::seconds(3661);
        CalibrationUiState calibration;
        calibration.session_id = "self-test";
        calibration.state = "signal_unstable";
        calibration.next_input_dbm = -20;
        const bool first_point_available = calibration_step_available(calibration);
        calibration.completed_points = 7;
        calibration.next_input_dbm = -55;
        const bool last_point_available = calibration_step_available(calibration);
        calibration.completed_points = 8;
        calibration.next_input_dbm.reset();
        CalibrationUiState high_gain;
        high_gain.band = "high";
        high_gain.selected_column = 2;
        const bool default_high_gain =
            selected_calibration_gain(high_gain) ==
            kDefaultHighCalibrationGainTenthsDb;
        high_gain.column_gain[2] = 520;
        const bool stored_high_gain = selected_calibration_gain(high_gain) == 520;
        high_gain.selected_column = 5;
        const bool independent_high_gain = selected_calibration_gain(high_gain) ==
            kDefaultHighCalibrationGainTenthsDb;
        high_gain.band = "low";
        const bool background_requests_do_not_block =
            is_background_request("读取 CH1") &&
            is_background_request("CAL query") &&
            is_background_request("启动状态订阅") &&
            !is_background_request("切换信道") &&
            is_switch_request("切换信道") &&
            is_switch_request("激活并保存") &&
            !is_switch_request("停止转发");
        bool quick_profile_persistence = false;
        const std::filesystem::path quick_test_path =
            std::filesystem::temp_directory_path() /
            "dmr-b210-gui-quick-profile-self-test.yaml";
        try {
            {
                std::ofstream output(quick_test_path);
                output << "gui:\n  profile_ids: [channel-01, channel-02, channel-03]\n";
            }
            persist_gui_quick_profiles(
                quick_test_path, {"channel-03", "channel-01", "channel-02"});
            const YAML::Node persisted = YAML::LoadFile(quick_test_path.string());
            const YAML::Node quick = persisted["gui"]["quick_profile_ids"];
            quick_profile_persistence =
                quick.IsSequence() && quick.size() == 3U &&
                quick[0].as<std::string>() == "channel-01" &&
                quick[1].as<std::string>() == "channel-02" &&
                quick[2].as<std::string>() == "channel-03";
        } catch (...) {
            quick_profile_persistence = false;
        }
        std::error_code quick_test_cleanup_error;
        std::filesystem::remove(quick_test_path, quick_test_cleanup_error);
        return level.label == "S9" && level.lit_segments == 9 &&
            s_meter(-67.0, -87.0).label == "S9 +20 dB" &&
            s_meter(-140.0, -87.0).lit_segments == 0 &&
            s_meter(kCalibratedS1Dbm, kCalibratedS9Dbm).label == "S1" &&
            elapsed_clock(started) == "01:01:01" &&
            kLowCalibrationInputDbm.front() == -20 &&
            kLowCalibrationInputDbm.back() == -55 &&
            kHighCalibrationInputDbm.back() == -125 &&
            first_point_available && last_point_available &&
            !calibration_step_available(calibration) &&
            default_high_gain && stored_high_gain && independent_high_gain &&
            selected_calibration_gain(high_gain) == 0 &&
            background_requests_do_not_block &&
            quick_profile_persistence;
    }

    static bool interaction_self_test(GuiConfig config)
    {
        if (config.profile_ids.empty()) {
            for (int index = 1; index <= 8; ++index) {
                config.profile_ids.push_back("channel-0" + std::to_string(index));
            }
        }
        if (config.quick_profile_ids.empty()) {
            config.quick_profile_ids = {"channel-01", "channel-02", "channel-03"};
        }
        if (config.font_path.empty()) {
            config.font_path = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
        }
        const std::filesystem::path test_path =
            std::filesystem::temp_directory_path() /
            "dmr-b210-gui-interaction-self-test.yaml";
        bool initialized = false;
        bool passed = false;
        try {
            {
                std::ofstream output(test_path);
                output << "gui:\n"
                       << "  profile_ids: [channel-01, channel-02, channel-03, channel-04, "
                          "channel-05, channel-06, channel-07, channel-08]\n"
                       << "  quick_profile_ids: [channel-01, channel-02, channel-03]\n";
            }
            config.config_path = test_path;
            config.listen_port = 0;
            config.udp_port = 42999;
            config.framebuffer_direct_output = false;
            config.rotation_degrees = 0;
            config.kmsdrm_device_index = -1;
            GuiApp app(std::move(config), false);
            app.initialize_sdl();
            initialized = true;
            app.state_.online = true;
            app.state_.stale = false;
            app.state_.forwarding_enabled = true;
            for (std::size_t index = 0; index < app.config_.profile_ids.size(); ++index) {
                Channel channel;
                channel.id = app.config_.profile_ids[index];
                channel.rx_frequency_hz = 145400000 + static_cast<std::int64_t>(index) * 12500;
                channel.tx_frequency_hz = 438500000 + static_cast<std::int64_t>(index) * 12500;
                app.channels_[channel.id] = channel;
            }
            app.state_.active_profile = "channel-01";
            app.state_.active_channel = app.channels_["channel-01"];
            app.update_runtime("{\"active_channel_profile_id\":\"channel-01\","
                               "\"forwarding_enabled\":true,\"rf_running\":false}");
            const bool no_premature_forwarding_event = std::none_of(
                app.events_.begin(), app.events_.end(), [](const EventLine& event) {
                    return event.message == "转发已激活，转发频道为CH1";
                });
            app.update_runtime("{\"active_channel_profile_id\":\"channel-01\","
                               "\"forwarding_enabled\":true,\"rf_running\":true}");
            const bool startup_forwarding_event = std::any_of(
                app.events_.begin(), app.events_.end(), [](const EventLine& event) {
                    return event.message == "转发已激活，转发频道为CH1";
                });

            const auto click = [&app](int x, int y) {
                app.last_pointer_up_ = {};
                app.dispatch_pointer_up(x, y);
                app.draw();
            };
            app.draw();
            click(228, 159);
            const std::string first_request = app.latest_switch_request_id_;
            const bool first_quick_switch = app.pending_switch_profile_ == "channel-02";
            click(362, 159);
            const std::string second_request = app.latest_switch_request_id_;
            const bool repeated_quick_switch = app.pending_switch_profile_ == "channel-03" &&
                !first_request.empty() && !second_request.empty() && first_request != second_request;

            app.update_response("{\"type\":\"response\",\"request_id\":\"" +
                first_request +
                "\",\"ok\":true,\"code\":\"accepted\",\"state\":{"
                "\"active_channel_profile_id\":\"channel-02\","
                "\"active_rx_frequency_hz\":145412500,"
                "\"active_tx_frequency_hz\":438512500}}");
            const bool old_response_ignored = app.state_.active_profile == "channel-01" &&
                app.pending_switch_profile_ == "channel-03";
            app.update_response("{\"type\":\"response\",\"request_id\":\"" +
                second_request +
                "\",\"ok\":true,\"code\":\"accepted\",\"state\":{"
                "\"active_channel_profile_id\":\"channel-03\","
                "\"active_rx_frequency_hz\":145425000,"
                "\"active_tx_frequency_hz\":438525000}}");
            const bool latest_response_applied = app.state_.active_profile == "channel-03" &&
                app.state_.active_channel.rx_frequency_hz == 145425000;

            click(300, 460);
            const bool channel_page_open = app.page_ == 1;
            click(42, 143);
            click(42, 341);
            const bool quick_configuration_changed =
                app.quick_profile_ids_ == std::vector<std::string>{
                    "channel-02", "channel-03", "channel-04"};
            click(220, 341);
            const bool non_quick_channel_opened = app.page_ == 2 &&
                app.detail_profile_id_ == "channel-04";
            click(485, 323);
            const bool detail_activation_sent = app.pending_switch_profile_ == "channel-04";
            app.update_runtime("{\"active_channel_profile_id\":\"channel-04\","
                               "\"active_rx_frequency_hz\":145437500,"
                               "\"active_tx_frequency_hz\":438537500}");
            const bool detail_activation_synchronized = app.state_.active_profile == "channel-04" &&
                app.state_.active_channel.rx_frequency_hz == 145437500;

            click(100, 460);
            click(94, 159);
            app.update_runtime("{\"active_channel_profile_id\":\"channel-02\","
                               "\"active_rx_frequency_hz\":145412500,"
                               "\"active_tx_frequency_hz\":438512500}");
            click(94, 159);
            click(228, 159);
            const bool active_card_then_other_card =
                app.pending_switch_profile_ == "channel-03";
            click(330, 215);
            const bool forwarding_control_remains_clickable =
                std::any_of(app.pending_.begin(), app.pending_.end(), [](const auto& item) {
                    return item.second == "停止转发";
                });
            std::string stop_request;
            for (const auto& item : app.pending_operations_) {
                if (item.second == "stop_forwarding") stop_request = item.first;
            }
            app.update_response("{\"type\":\"response\",\"request_id\":\"" +
                stop_request + "\",\"ok\":true,\"code\":\"accepted\"}");
            app.update_runtime("{\"active_channel_profile_id\":\"channel-03\","
                               "\"forwarding_enabled\":false,\"rf_running\":false}");
            const bool forwarding_result_event = std::any_of(
                app.events_.begin(), app.events_.end(), [](const EventLine& event) {
                    return event.message == "转发已停止";
                });

            const YAML::Node persisted = YAML::LoadFile(test_path.string());
            const YAML::Node quick = persisted["gui"]["quick_profile_ids"];
            const bool quick_configuration_persisted = quick.IsSequence() && quick.size() == 3U &&
                quick[0].as<std::string>() == "channel-02" &&
                quick[1].as<std::string>() == "channel-03" &&
                quick[2].as<std::string>() == "channel-04";
            passed = no_premature_forwarding_event && startup_forwarding_event &&
                first_quick_switch && repeated_quick_switch && old_response_ignored &&
                latest_response_applied && channel_page_open && quick_configuration_changed &&
                non_quick_channel_opened && detail_activation_sent &&
                detail_activation_synchronized && active_card_then_other_card &&
                forwarding_control_remains_clickable && forwarding_result_event &&
                quick_configuration_persisted;
            app.shutdown_sdl();
            initialized = false;
        } catch (const std::exception& error) {
            std::cerr << "GUI interaction self-test: " << error.what() << '\n';
        }
        std::error_code cleanup_error;
        std::filesystem::remove(test_path, cleanup_error);
        if (initialized) {
            // Initialization failures are reported above; process teardown releases SDL.
        }
        return passed;
    }

    void configure_qa_view(const std::string& view)
    {
        qa_view_enabled_ = true;
        if (view == "home") {
            page_ = 0;
        } else if (view == "channels") {
            page_ = 1;
            channel_page_ = 0;
        } else if (view == "detail") {
            page_ = 1;
            channel_page_ = 1;
            if (!config_.profile_ids.empty()) {
                detail_profile_id_ = config_.profile_ids.front();
            }
        } else if (view == "parameters") {
            page_ = 2;
        } else if (view == "status") {
            page_ = 3;
        } else if (view == "password") {
            page_ = 4;
            calibration_.authorized = false;
        } else if (view == "calibration" || view == "dialog") {
            page_ = 4;
            calibration_.authorized = true;
            calibration_.state = "active";
            calibration_.session_id = "qa-layout";
            calibration_.next_input_dbm = kLowCalibrationInputDbm.front();
            if (view == "dialog") {
                calibration_.completed_points = 5;
                calibration_.next_input_dbm = kLowCalibrationInputDbm[5];
                calibration_leave_dialog_ = true;
            }
        } else {
            throw std::runtime_error("unknown QA view: " + view);
        }
    }

private:
    struct Meter {
        int lit_segments = 0;
        std::string label;
    };

    static Meter s_meter(double rssi, double s9_reference)
    {
        const double delta = rssi - s9_reference;
        Meter meter;
        if (delta < -48.0) {
            meter.label = "S1";
            return meter;
        }
        if (delta < 0.0) {
            meter.lit_segments = std::min(9, static_cast<int>(std::floor((delta + 48.0) / 6.0)) + 1);
            meter.label = "S" + std::to_string(meter.lit_segments);
            return meter;
        }
        meter.lit_segments = std::min(12, 9 + static_cast<int>(std::floor(delta / 20.0)) + 1);
        if (delta < 1.0) {
            meter.lit_segments = 9;
            meter.label = "S9";
        } else {
            meter.label = "S9 +" + std::to_string(static_cast<int>(std::floor(delta))) + " dB";
        }
        return meter;
    }

    void initialize_sdl()
    {
        if (config_.framebuffer_direct_output &&
            SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) != 0) {
            throw std::runtime_error("cannot select direct framebuffer video driver");
        }
        if (config_.kmsdrm_device_index >= 0 &&
            !config_.framebuffer_direct_output &&
            SDL_setenv("SDL_KMSDRM_DEVICE_INDEX",
                       std::to_string(config_.kmsdrm_device_index).c_str(),
                       1) != 0) {
            throw std::runtime_error("cannot select configured KMSDRM device");
        }
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0 || TTF_Init() != 0) {
            throw std::runtime_error("cannot initialize SDL kiosk runtime");
        }
        window_ = SDL_CreateWindow("DMR B210", SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight,
            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_BORDERLESS);
        if (!window_) throw std::runtime_error("cannot create kiosk window");
        const char* video_driver = SDL_GetCurrentVideoDriver();
        const bool kmsdrm = video_driver && std::string(video_driver) == "kmsdrm";
        if (kmsdrm) {
            // Pi5 uses separate V3D render and RP1 DSI DRM devices. Rendering
            // through EGL can leave the DSI plane black even though KMS owns it.
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        } else {
            renderer_ = SDL_CreateRenderer(window_, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        if (!renderer_ && kmsdrm) {
            renderer_ = SDL_CreateRenderer(window_, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        }
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) throw std::runtime_error("cannot create kiosk renderer");
        SDL_RendererInfo renderer_info{};
        if (SDL_GetRendererInfo(renderer_, &renderer_info) == 0) {
            std::cerr << "dmr_b210_gui: SDL renderer=" << renderer_info.name
                      << " video=" << (video_driver ? video_driver : "unknown")
                      << '\n';
        }
        canvas_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, kWidth, kHeight);
        if (!canvas_) throw std::runtime_error("cannot create kiosk canvas");
#if defined(__linux__)
        if (config_.framebuffer_direct_output) {
            const std::string framebuffer_path = select_dsi1_framebuffer(config_.framebuffer_path);
            framebuffer_fd_ = ::open(framebuffer_path.c_str(), O_RDWR | O_CLOEXEC);
            if (framebuffer_fd_ < 0) {
                throw std::runtime_error("cannot open configured framebuffer");
            }
            if (ioctl(framebuffer_fd_, FBIOBLANK, FB_BLANK_UNBLANK) != 0) {
                std::cerr << "dmr_b210_gui: cannot unblank direct framebuffer\n";
            }
            if (ioctl(framebuffer_fd_, FBIOGET_FSCREENINFO, &framebuffer_fix_) != 0 ||
                ioctl(framebuffer_fd_, FBIOGET_VSCREENINFO, &framebuffer_var_) != 0 ||
                framebuffer_var_.bits_per_pixel != 32 ||
                framebuffer_var_.xres < static_cast<unsigned>(kWidth) ||
                framebuffer_var_.yres < static_cast<unsigned>(kHeight)) {
                throw std::runtime_error("unsupported configured framebuffer format");
            }
            framebuffer_map_ = static_cast<std::uint8_t*>(mmap(
                nullptr, framebuffer_fix_.smem_len, PROT_READ | PROT_WRITE,
                MAP_SHARED, framebuffer_fd_, 0));
            if (framebuffer_map_ == MAP_FAILED) {
                framebuffer_map_ = nullptr;
                throw std::runtime_error("cannot map configured framebuffer");
            }
            framebuffer_pixels_.resize(static_cast<std::size_t>(kWidth) *
                                       static_cast<std::size_t>(kHeight) * 4U);
            direct_framebuffer_output_ = true;
            std::cerr << "dmr_b210_gui: direct framebuffer=" << framebuffer_path << '\n';
            open_touch_input();
        }
#endif
        font_ = TTF_OpenFont(config_.font_path.c_str(), 16);
        if (!font_) throw std::runtime_error("cannot open configured Chinese font");
        font_small_ = TTF_OpenFont(config_.font_path.c_str(), 13);
        font_bold_ = TTF_OpenFont(config_.font_path.c_str(), 19);
        font_frequency_ = TTF_OpenFont(config_.font_path.c_str(), 25);
        font_frequency_digits_ = TTF_OpenFont(config_.frequency_digit_font_path.c_str(), 31);
        if (!font_frequency_digits_) {
            std::cerr << "dmr_b210_gui: frequency digit font unavailable, using kiosk font: "
                      << config_.frequency_digit_font_path << '\n';
            font_frequency_digits_ = TTF_OpenFont(config_.font_path.c_str(), 31);
        }
        if (!font_small_ || !font_bold_ || !font_frequency_ || !font_frequency_digits_) {
            throw std::runtime_error("cannot open kiosk font sizes");
        }
    }

    void shutdown_sdl()
    {
#if defined(__linux__)
        if (framebuffer_map_) {
            munmap(framebuffer_map_, framebuffer_fix_.smem_len);
            framebuffer_map_ = nullptr;
        }
        if (framebuffer_fd_ >= 0) {
            ::close(framebuffer_fd_);
            framebuffer_fd_ = -1;
        }
        if (touch_fd_ >= 0) {
            ::close(touch_fd_);
            touch_fd_ = -1;
        }
#endif
        if (font_bold_) TTF_CloseFont(font_bold_);
        if (font_frequency_) TTF_CloseFont(font_frequency_);
        if (font_frequency_digits_) TTF_CloseFont(font_frequency_digits_);
        if (font_small_) TTF_CloseFont(font_small_);
        if (font_) TTF_CloseFont(font_);
        if (canvas_) SDL_DestroyTexture(canvas_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        TTF_Quit();
        SDL_Quit();
    }

    int stop_forwarding()
    {
        try {
            const std::string id = client_.send("stop_forwarding");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                for (const std::string& frame : client_.receive()) {
                    if (json_string(frame, "request_id").value_or("") == id &&
                        json_bool(frame, "ok").value_or(false)) return 0;
                }
                SDL_Delay(20);
            }
        } catch (...) {
        }
        return 1;
    }

    void send_initial_queries()
    {
        last_connection_attempt_ = std::chrono::steady_clock::now();
        track(client_.send("get_version"), "读取中继版本");
        track(client_.send("get_status"), "读取运行状态");
        track(client_.send("get_channel"), "读取活动信道");
        track(client_.send("get_gain_mode"), "读取接收增益");
        track(client_.send("get_rx_calibration"), "CAL query");
        track(client_.send("start_status_query",
            "\"listen_port\":" + std::to_string(config_.listen_port)), "启动状态订阅");
    }

    void stop_status_subscription()
    {
        try {
            client_.send("stop_status_query");
        } catch (...) {
        }
    }

    void track(const std::string& id, const std::string& action,
               const std::string& operation = {})
    {
        pending_[id] = action;
        pending_sent_at_[id] = std::chrono::steady_clock::now();
        if (!operation.empty()) pending_operations_[id] = operation;
    }

    void expire_pending_requests()
    {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_sent_at_.begin(); it != pending_sent_at_.end();) {
            if (now - it->second <= std::chrono::milliseconds(1500)) {
                ++it;
                continue;
            }
            const auto pending = pending_.find(it->first);
            if (pending != pending_.end()) {
                const std::string action = pending->second;
                const std::string operation = pending_operations_.count(it->first) != 0U
                    ? pending_operations_[it->first] : "";
                pending_.erase(pending);
                pending_operations_.erase(it->first);
                channel_requests_.erase(it->first);
                const auto switch_request = switch_requests_.find(it->first);
                const bool superseded_switch = operation == "switch_channel" &&
                    it->first != latest_switch_request_id_;
                if (switch_request != switch_requests_.end()) {
                    switch_requests_.erase(switch_request);
                }
                if (action != "CAL query") {
                    add_event(superseded_switch
                        ? action + "：已由新请求替代"
                        : action + "失败：响应超时",
                        superseded_switch ? kAmber : kRed);
                }
                if (it->first == pending_forwarding_request_id_) {
                    pending_forwarding_target_.reset();
                    pending_forwarding_request_id_.clear();
                    pending_forwarding_action_.clear();
                    pending_forwarding_accepted_ = false;
                }
                if (it->first == latest_switch_request_id_) {
                    clear_switch_confirmation();
                }
            }
            it = pending_sent_at_.erase(it);
        }
        expire_state_confirmations(now);
        maybe_announce_startup_forwarding();
    }

    void clear_switch_confirmation()
    {
        pending_switch_profile_.reset();
        pending_switch_action_.clear();
        pending_switch_accepted_ = false;
        pending_switch_state_confirmed_ = false;
        latest_switch_request_id_.clear();
    }

    void expire_state_confirmations(std::chrono::steady_clock::time_point now)
    {
        if (pending_forwarding_target_ && pending_forwarding_accepted_ &&
            now - pending_forwarding_started_ > std::chrono::seconds(8)) {
            add_event(pending_forwarding_action_ + "失败：状态确认超时", kRed);
            pending_forwarding_target_.reset();
            pending_forwarding_request_id_.clear();
            pending_forwarding_action_.clear();
            pending_forwarding_accepted_ = false;
        }
        if (pending_switch_profile_ && pending_switch_accepted_ &&
            now - pending_switch_started_ > std::chrono::seconds(8)) {
            add_event(pending_switch_action_ + "失败：状态确认超时", kRed);
            clear_switch_confirmation();
        }
    }

    void announce_forwarding_active()
    {
        add_event("转发已激活，转发频道为" + channel_label(state_.active_profile), kGreen);
        startup_forwarding_announced_ = true;
    }

    bool initial_queries_pending() const
    {
        return std::any_of(pending_.begin(), pending_.end(), [](const auto& item) {
            return item.second == "启动状态订阅" ||
                item.second.rfind("读取", 0) == 0;
        });
    }

    void maybe_announce_startup_forwarding()
    {
        if (startup_forwarding_announced_ || !state_.online ||
            !forwarding_enabled_known_ || !rf_running_known_ ||
            state_.active_profile.empty() || initial_queries_pending()) {
            return;
        }
        if (state_.forwarding_enabled && state_.rf_running) {
            announce_forwarding_active();
        }
    }

    void evaluate_forwarding_confirmation()
    {
        if (!pending_forwarding_target_ || !pending_forwarding_accepted_ ||
            !forwarding_enabled_known_ || !rf_running_known_) {
            return;
        }
        if (!state_.last_error.empty()) {
            add_event(pending_forwarding_action_ + "失败：" + state_.last_error, kRed);
        } else {
            const bool reached = *pending_forwarding_target_
                ? state_.forwarding_enabled && state_.rf_running
                : !state_.forwarding_enabled && !state_.rf_running;
            if (!reached) return;
            if (*pending_forwarding_target_) announce_forwarding_active();
            else add_event("转发已停止", kGreen);
        }
        pending_forwarding_target_.reset();
        pending_forwarding_request_id_.clear();
        pending_forwarding_action_.clear();
        pending_forwarding_accepted_ = false;
    }

    void evaluate_switch_confirmation()
    {
        if (!pending_switch_profile_ || !pending_switch_accepted_ ||
            !pending_switch_state_confirmed_) {
            return;
        }
        if (!state_.last_error.empty()) {
            add_event(pending_switch_action_ + "失败：" + state_.last_error, kRed);
        } else {
            add_event(pending_switch_action_ + "成功：" +
                      channel_label(*pending_switch_profile_), kGreen);
        }
        clear_switch_confirmation();
    }

    void retry_controller_connection()
    {
        if (!state_.stale && state_.online) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_connection_attempt_ < std::chrono::seconds(1)) return;
        add_event("重试中继连接", kAmber);
        send_initial_queries();
    }

    void add_event(std::string message, SDL_Color color)
    {
        constexpr std::size_t kMaximumEvents = 4;
        if (message.size() > 54U) message.resize(54U);
        events_.insert(events_.begin(), {now_clock(), std::move(message), color});
        if (events_.size() > kMaximumEvents) events_.resize(kMaximumEvents);
    }

    void handle_network()
    {
        for (const std::string& frame : client_.receive()) {
            const std::string type = json_string(frame, "type").value_or("");
            if (type == "status_stream") {
                update_status_stream(frame);
            } else if (type == "response") {
                update_response(frame);
            }
        }
        expire_pending_requests();
        refresh_due_channels();
        refresh_calibration_page();
        refresh_pending_calibration_exit();
        const auto now = std::chrono::steady_clock::now();
        const bool stale = state_.last_update.time_since_epoch().count() == 0 ||
            now - state_.last_update > std::chrono::seconds(1);
        if (stale != state_.stale) {
            state_.stale = stale;
            if (stale) {
                state_.active_call = {};
                startup_forwarding_announced_ = false;
                forwarding_enabled_known_ = false;
                rf_running_known_ = false;
                add_event("状态订阅已中断", kRed);
            }
        }
        state_.online = !state_.stale;
        retry_controller_connection();
    }

    void update_runtime(const std::string& object)
    {
        const auto runtime_error = json_string(object, "last_error").value_or("");
        if (const auto profile = json_string(object, "active_channel_profile_id")) {
            bool accept_profile = true;
            if (pending_switch_profile_) {
                if (*profile == *pending_switch_profile_) {
                    pending_switch_state_confirmed_ = true;
                } else if (!runtime_error.empty() ||
                           std::chrono::steady_clock::now() - pending_switch_started_ >
                               std::chrono::seconds(8)) {
                    add_event(pending_switch_action_ + "失败：" +
                              (runtime_error.empty() ? "状态确认超时" : runtime_error), kRed);
                    clear_switch_confirmation();
                } else {
                    // RF reinitialization is asynchronous. Do not let an old
                    // status packet roll the GUI back while the target RF
                    // profile is still being activated.
                    accept_profile = false;
                }
            }
            if (accept_profile) {
                const bool changed = *profile != state_.active_profile;
                state_.active_profile = *profile;
                const auto cached_channel = channels_.find(*profile);
                Channel active_channel = cached_channel == channels_.end()
                    ? state_.active_channel : cached_channel->second;
                active_channel.id = *profile;
                const auto active_rx = json_number<std::int64_t>(
                    object, "active_rx_frequency_hz");
                const auto active_tx = json_number<std::int64_t>(
                    object, "active_tx_frequency_hz");
                if (active_rx && *active_rx > 0) {
                    active_channel.rx_frequency_hz = *active_rx;
                }
                if (active_tx && *active_tx > 0) {
                    active_channel.tx_frequency_hz = *active_tx;
                }
                state_.active_channel = active_channel;
                if (active_channel.rx_frequency_hz > 0 &&
                    active_channel.tx_frequency_hz > 0) {
                    channels_[*profile] = active_channel;
                }
                if (changed) {
                    if (state_.online) {
                        request_channel_refresh(*profile);
                    } else {
                        channel_refresh_due_[*profile] =
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
                    }
                }
            }
        }
        if (const auto count = json_number<int>(object, "configured_channel_profile_count")) state_.configured_profiles = *count;
        if (const auto bytes = json_number<std::uint64_t>(object, "recording_storage_bytes")) {
            state_.recording_storage_bytes = *bytes;
        }
        if (const auto limit = json_number<std::uint64_t>(object, "recording_storage_limit_bytes")) {
            state_.recording_storage_limit_bytes = *limit;
        }
        if (const auto forwarding = json_bool(object, "forwarding_enabled")) {
            state_.forwarding_enabled = *forwarding;
            forwarding_enabled_known_ = true;
        }
        if (const auto running = json_bool(object, "rf_running")) {
            state_.rf_running = *running;
            rf_running_known_ = true;
        }
        if (const auto fault = json_bool(object, "rf_fault")) state_.rf_fault = *fault;
        state_.last_error = runtime_error;
        if (const auto version = json_string(object, "repeater_version")) {
            state_.repeater_version = *version;
        }
        if (const auto build = json_number<int>(object, "build_sequence")) {
            state_.repeater_build_sequence = *build;
        }
        const std::string gain = json_object(object, "gain_control");
        if (!gain.empty()) {
            if (const auto mode = json_string(gain, "active_mode")) {
                state_.gain_mode = *mode;
            } else if (const auto mode = json_string(gain, "mode")) {
                state_.gain_mode = *mode;
            }
            if (const auto selection = json_string(gain, "selection_mode")) {
                state_.gain_selection_mode = *selection;
            }
        }
        evaluate_switch_confirmation();
        evaluate_forwarding_confirmation();
        maybe_announce_startup_forwarding();
    }

    void update_channel(const std::string& object, bool make_active)
    {
        Channel channel;
        channel.id = json_string(object, "id").value_or(state_.active_profile);
        const std::string rx = json_object(object, "dmr_rx");
        const std::string tx = json_object(object, "dmr_tx");
        const std::string fm = json_object(object, "fm");
        if (!rx.empty()) {
            channel.rx_frequency_hz = json_number<std::int64_t>(rx, "frequency_hz").value_or(0);
            channel.rx_gain_tenths_db = json_number<int>(rx, "gain_tenths_db").value_or(0);
        }
        if (!tx.empty()) {
            channel.tx_frequency_hz = json_number<std::int64_t>(tx, "frequency_hz").value_or(0);
            channel.tx_gain_tenths_db = json_number<int>(tx, "gain_tenths_db").value_or(0);
        }
        if (!fm.empty()) {
            channel.fm_enabled = json_bool(fm, "enabled").value_or(false);
            channel.ctcss_tone_tenths_hz = json_number<int>(fm, "ctcss_tone_tenths_hz").value_or(1230);
        }
        if (channel.rx_frequency_hz > 0 && channel.tx_frequency_hz > 0) {
            channels_[channel.id] = channel;
            if (make_active) {
                state_.active_channel = channel;
                state_.active_profile = channel.id;
            }
            if (channel.id == draft_profile_id_ && !draft_dirty_) {
                draft_channel_ = channel;
            }
        }
    }

    void update_status_stream(const std::string& frame)
    {
        const auto sequence = json_number<std::uint64_t>(frame, "sequence");
        if (!sequence || *sequence <= state_.last_sequence) return;
        state_.last_sequence = *sequence;
        state_.last_update = std::chrono::steady_clock::now();
        state_.stale = false;
        state_.online = true;
        if (const auto mode = json_string(frame, "working_mode")) state_.working_mode = *mode;
        const std::string runtime = json_object(frame, "runtime");
        if (!runtime.empty()) update_runtime(runtime);
        std::array<bool, 2> receiver_seen{};
        for (int channel = 0; channel < 2; ++channel) {
            const std::string needle = "\"rx_channel\":" + std::to_string(channel);
            const std::size_t begin = frame.find(needle);
            if (begin == std::string::npos) continue;
            const std::size_t end = frame.find('}', begin);
            const std::string receiver = frame.substr(begin, end == std::string::npos ? std::string::npos : end - begin + 1U);
            auto& next = state_.receivers[static_cast<std::size_t>(channel)];
            receiver_seen[static_cast<std::size_t>(channel)] = true;
            next.receiving = json_bool(receiver, "receiving").value_or(false);
            next.mode = json_string(receiver, "receiver_mode").value_or("未知");
            next.rssi_valid = receiver.find("\"rssi_dbfs\":null") == std::string::npos;
            next.rssi_dbm_valid = receiver.find("\"rssi_dbm\":null") == std::string::npos;
            next.snr_valid = receiver.find("\"snr_db\":null") == std::string::npos;
            if (next.rssi_valid) next.rssi_dbfs = json_number<double>(receiver, "rssi_dbfs").value_or(0.0);
            if (next.rssi_dbm_valid) next.rssi_dbm = json_number<double>(receiver, "rssi_dbm").value_or(0.0);
            if (next.snr_valid) next.snr_db = json_number<double>(receiver, "snr_db").value_or(0.0);
            next.calibration_state = json_string(receiver, "calibration_state").value_or("uncalibrated");
            next.hardware_agc_enabled = json_bool(receiver, "hardware_agc_enabled").value_or(false);
            next.analog_gain_valid = receiver.find("\"analog_gain_db\":null") == std::string::npos;
            next.software_agc_gain_valid = receiver.find("\"software_agc_gain_db\":null") == std::string::npos;
            next.agc_input_valid = receiver.find("\"agc_input_dbfs\":null") == std::string::npos;
            next.rssi_gain_compensation_valid = receiver.find("\"rssi_gain_compensation_db\":null") == std::string::npos;
            if (next.analog_gain_valid) next.analog_gain_db = json_number<double>(receiver, "analog_gain_db").value_or(0.0);
            if (next.software_agc_gain_valid) next.software_agc_gain_db = json_number<double>(receiver, "software_agc_gain_db").value_or(0.0);
            if (next.agc_input_valid) next.agc_input_dbfs = json_number<double>(receiver, "agc_input_dbfs").value_or(0.0);
            if (next.rssi_gain_compensation_valid) next.rssi_gain_compensation_db = json_number<double>(receiver, "rssi_gain_compensation_db").value_or(0.0);
        }
        for (std::size_t channel = 0; channel < receiver_seen.size(); ++channel) {
            if (!receiver_seen[channel]) state_.receivers[channel] = {};
        }
        const auto active_position = json_value_position(frame, "active_call");
        if (!active_position || frame.compare(*active_position, 4U, "null") == 0) {
            state_.active_call = {};
        } else {
            const std::string active = json_object(frame, "active_call");
            ActiveCall call;
            call.valid = !active.empty();
            call.rx_channel = json_number<int>(active, "rx_channel").value_or(-1);
            call.source_id = json_number<std::uint32_t>(active, "source_id").value_or(0);
            call.destination_id = json_number<std::uint32_t>(active, "destination_id").value_or(0);
            call.mode = json_string(active, "mode").value_or("未知");
            if (!state_.active_call.valid || state_.active_call.source_id != call.source_id ||
                state_.active_call.destination_id != call.destination_id || state_.active_call.mode != call.mode) {
                active_call_started_ = std::chrono::steady_clock::now();
            }
            state_.active_call = call;
        }
    }

    void update_calibration_state(const std::string& object)
    {
        const std::string curves = json_array(object, "curves");
        for (const std::string& curve : json_array_objects(curves)) {
            const int rx_channel = json_number<int>(curve, "rx_channel").value_or(-1);
            const std::string band = json_string(curve, "band").value_or("");
            if (rx_channel < 0 || rx_channel > 1 ||
                (band != "low" && band != "medium" && band != "high")) continue;
            const int column = rx_channel * 3 + calibration_band_index(band);
            if (calibration_.column_edited[static_cast<std::size_t>(column)]) {
                continue;
            }
            calibration_.column_gain[static_cast<std::size_t>(column)] =
                json_number<int>(curve, "rx_gain_tenths_db");
            calibration_.column_points[static_cast<std::size_t>(column)].clear();
            for (const std::string& point : json_array_objects(json_array(curve, "points"))) {
                const auto input = json_number<int>(point, "input_dbm");
                const auto dbfs = json_number<double>(point, "measured_dbfs");
                const auto snr = json_number<double>(point, "snr_db");
                if (input && dbfs && snr) {
                    calibration_.column_points[static_cast<std::size_t>(column)][*input] =
                        {*dbfs, *snr};
                }
            }
        }
        const std::string session_points = json_array(object, "points");
        const int session_column = calibration_.rx_channel * 3 +
            calibration_band_index(calibration_.band);
        if (session_column >= 0 && session_column < 6 && !session_points.empty()) {
            auto& points = calibration_.column_points[static_cast<std::size_t>(session_column)];
            for (const std::string& point : json_array_objects(session_points)) {
                const auto input = json_number<int>(point, "input_dbm");
                const auto dbfs = json_number<double>(point, "measured_dbfs");
                const auto snr = json_number<double>(point, "snr_db");
                if (input && dbfs && snr) points[*input] = {*dbfs, *snr};
            }
        }
    }

    void update_response(const std::string& frame)
    {
        const std::string request_id = json_string(frame, "request_id").value_or("");
        const bool silent_calibration_query = calibration_query_request_ &&
            *calibration_query_request_ == request_id;
        if (silent_calibration_query) calibration_query_request_.reset();
        const auto channel_request = channel_requests_.find(request_id);
        const std::string requested_profile = channel_request == channel_requests_.end() ? "" :
            channel_request->second;
        if (channel_request != channel_requests_.end()) channel_requests_.erase(channel_request);
        const auto pending = pending_.find(request_id);
        const std::string action = pending == pending_.end() ? "UDP 命令" : pending->second;
        if (pending != pending_.end()) pending_.erase(pending);
        const auto pending_operation = pending_operations_.find(request_id);
        const std::string operation = pending_operation == pending_operations_.end()
            ? "" : pending_operation->second;
        if (pending_operation != pending_operations_.end()) {
            pending_operations_.erase(pending_operation);
        }
        const auto switch_request = switch_requests_.find(request_id);
        const std::string switched_profile = switch_request == switch_requests_.end()
            ? "" : switch_request->second;
        if (switch_request != switch_requests_.end()) {
            switch_requests_.erase(switch_request);
        }
        pending_sent_at_.erase(request_id);
        const bool ok = json_bool(frame, "ok").value_or(false);
        const std::string code = json_string(frame, "code").value_or("错误");
        const std::string message = json_string(frame, "message").value_or("");
        if (!silent_calibration_query) {
            const bool superseded_switch = operation == "switch_channel" &&
                request_id != latest_switch_request_id_;
            const bool waits_for_runtime = operation == "start_forwarding" ||
                operation == "stop_forwarding" || operation == "switch_channel";
            if (!ok) {
                add_event(action + "失败：" + (message.empty() ? code : message), kRed);
            } else if (superseded_switch) {
                add_event(action + "：已由新请求替代", kAmber);
            } else if (waits_for_runtime) {
                add_event(action + "：已受理，等待状态", kAmber);
            } else {
                add_event(action + "成功：" + (message.empty() ? code : message), kGreen);
            }
        }
        const std::string state = json_object(frame, "state");
        if (!state.empty()) {
            const bool calibration_state =
                state.find("\"state\"") != std::string::npos &&
                state.find("\"dmr_rx\"") == std::string::npos;
            if (calibration_state && !qa_view_enabled_) {
                calibration_.state = json_string(state, "state").value_or("idle");
                const std::string session_id = json_string(state, "session_id").value_or("");
                calibration_.session_id = session_id;
                if (!session_id.empty()) {
                    calibration_.band = json_string(state, "band").value_or(calibration_.band);
                    calibration_.rx_channel = json_number<int>(state, "rx_channel").value_or(calibration_.rx_channel);
                    calibration_.selected_column = calibration_.rx_channel * 3 +
                        calibration_band_index(calibration_.band);
                }
                calibration_.next_input_dbm = json_number<int>(state, "next_input_dbm");
                calibration_.completed_points = json_number<int>(state, "completed_points").value_or(0);
                if (!session_id.empty()) {
                    calibration_.gain_tenths_db =
                        json_number<int>(state, "rx_gain_tenths_db").value_or(
                            selected_calibration_gain(calibration_));
                } else {
                    calibration_.gain_tenths_db =
                        selected_calibration_gain(calibration_);
                }
                if (session_id.empty() &&
                    (calibration_.state == "cancelled" ||
                     calibration_.state == "committed")) {
                    const int completed_rx = json_number<int>(state, "rx_channel")
                        .value_or(calibration_.rx_channel);
                    const std::string completed_band = json_string(state, "band")
                        .value_or(calibration_.band);
                    const int completed_column = completed_rx * 3 +
                        calibration_band_index(completed_band);
                    if (completed_column >= 0 && completed_column < 6) {
                        calibration_.column_edited[
                            static_cast<std::size_t>(completed_column)] = false;
                    }
                }
            }
            if (!calibration_state || !qa_view_enabled_) {
                update_calibration_state(state);
            }
            if (request_id.find("channel") != std::string::npos ||
                state.find("\"dmr_rx\"") != std::string::npos) {
                update_channel(state, requested_profile.empty() ||
                    requested_profile == state_.active_profile);
            }
            update_runtime(state);
            if (action == "CAL save" && ok &&
                json_bool(state, "config_written").value_or(false)) {
                add_event("校准已保存", kGreen);
            }
        }
        const bool is_latest_switch_response = !switched_profile.empty() &&
            request_id == latest_switch_request_id_;
        if (ok && is_latest_switch_response) {
            pending_switch_accepted_ = true;
            // The switch command is queued for the RF owner. Apply the
            // cached channel immediately, then refresh from authoritative RF
            // status so every page follows the same active profile.
            state_.active_profile = switched_profile;
            const auto cached = channels_.find(switched_profile);
            if (cached != channels_.end()) {
                state_.active_channel = cached->second;
                state_.active_channel.id = switched_profile;
            } else {
                state_.active_channel.id = switched_profile;
            }
            request_channel_refresh(switched_profile);
            try {
                const std::string status_request = client_.send("get_status");
                track(status_request, "读取活动信道");
            } catch (const std::exception& error) {
                add_event(std::string("读取活动信道失败：") + error.what(), kRed);
            }
            evaluate_switch_confirmation();
        } else if (!ok && is_latest_switch_response) {
            clear_switch_confirmation();
        }
        if (request_id == pending_forwarding_request_id_) {
            if (ok) {
                pending_forwarding_accepted_ = true;
                evaluate_forwarding_confirmation();
            } else {
                pending_forwarding_target_.reset();
                pending_forwarding_request_id_.clear();
                pending_forwarding_action_.clear();
                pending_forwarding_accepted_ = false;
            }
        }
        if (calibration_exit_after_save_ && calibration_.state == "committed" &&
            calibration_.session_id.empty()) {
            finish_calibration_exit();
        } else if (calibration_exit_after_discard_ &&
                   (calibration_.state == "cancelled" || calibration_.state == "idle") &&
                   calibration_.session_id.empty()) {
            finish_calibration_exit();
        }
        if (action == "CAL begin" || action == "CAL submit" ||
            action == "CAL save" || action == "CAL cancel" ||
            action == "CAL gain" || action == "CAL auto") {
            request_calibration_refresh();
        }
        maybe_announce_startup_forwarding();
    }

    static bool is_background_request(const std::string& action)
    {
        return action == "CAL query" || action == "启动状态订阅" ||
            action.rfind("读取", 0) == 0;
    }

    static bool is_switch_request(const std::string& action)
    {
        return action == "切换信道" || action == "激活并保存" ||
            action == "设置开机信道";
    }

    bool controls_enabled() const
    {
        if (!state_.online) return false;
        return std::none_of(pending_.begin(), pending_.end(), [](const auto& item) {
            return !is_background_request(item.second) && !is_switch_request(item.second);
        });
    }

    void send_control(const std::string& operation, const std::string& fields,
                      const std::string& description)
    {
        if (!controls_enabled()) return;
        try {
            const std::string request_id = client_.send(operation, fields);
            track(request_id, description, operation);
            if (operation == "start_forwarding" || operation == "stop_forwarding") {
                pending_forwarding_target_ = operation == "start_forwarding";
                pending_forwarding_request_id_ = request_id;
                pending_forwarding_action_ = description;
                pending_forwarding_accepted_ = false;
                pending_forwarding_started_ = std::chrono::steady_clock::now();
            }
            add_event(description + "：等待确认", kAmber);
        } catch (const std::exception& error) {
            add_event(std::string("UDP 发送失败：") + error.what(), kRed);
        }
    }

    void draw_text(const std::string& text, int x, int y, SDL_Color color,
                   TTF_Font* font = nullptr)
    {
        TTF_Font* selected = font ? font : font_;
        SDL_Surface* surface = TTF_RenderUTF8_Blended(selected, text.c_str(), color);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_Rect target{x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer_, texture, nullptr, &target);
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
    }

    void draw_text_clipped(const std::string& text, int x, int y, SDL_Color color,
                           SDL_Rect clip, TTF_Font* font = nullptr)
    {
        TTF_Font* selected = font ? font : font_;
        SDL_Surface* surface = TTF_RenderUTF8_Blended(selected, text.c_str(), color);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_Rect target{x, y, surface->w, surface->h};
        SDL_RenderSetClipRect(renderer_, &clip);
        SDL_RenderCopy(renderer_, texture, nullptr, &target);
        SDL_RenderSetClipRect(renderer_, nullptr);
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
    }

    void draw_text_scaled_clipped(const std::string& text, SDL_Rect target,
                                  SDL_Color color, SDL_Rect clip,
                                  TTF_Font* font = nullptr)
    {
        TTF_Font* selected = font ? font : font_;
        SDL_Surface* surface = TTF_RenderUTF8_Blended(selected, text.c_str(), color);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        if (!texture) {
            SDL_FreeSurface(surface);
            return;
        }
        SDL_RenderSetClipRect(renderer_, &clip);
        SDL_RenderCopy(renderer_, texture, nullptr, &target);
        SDL_RenderSetClipRect(renderer_, nullptr);
        SDL_DestroyTexture(texture);
        SDL_FreeSurface(surface);
    }

    void draw_text_vcentered_clipped(const std::string& text, int x,
                                     SDL_Color color, SDL_Rect box,
                                     TTF_Font* font = nullptr)
    {
        TTF_Font* selected = font ? font : font_;
        int width = 0;
        int height = 0;
        TTF_SizeUTF8(selected, text.c_str(), &width, &height);
        const int y = box.y + std::max(0, (box.h - height) / 2);
        draw_text_clipped(text, x, y, color, box, selected);
    }

    void draw_home_frequency(const std::string& label, std::int64_t value,
                             int label_x, int digits_x, int unit_x)
    {
        const std::string digits = frequency_numeric_text(value);
        draw_text_clipped(label, label_x, 95, kCyan,
                          {label_x, 91, 42, 36}, font_bold_);
        draw_text_scaled_clipped(digits, {digits_x, 52, 120, 64}, kCyan,
                                 {digits_x, 45, 132, 88}, font_frequency_digits_);
        draw_text_clipped("MHz", unit_x, 101, kCyan,
                          {unit_x, 94, 42, 36}, font_);
    }

    void draw_box(SDL_Rect box, SDL_Color fill = kPanel)
    {
        SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, fill.a);
        SDL_RenderFillRect(renderer_, &box);
        SDL_SetRenderDrawColor(renderer_, kLine.r, kLine.g, kLine.b, kLine.a);
        SDL_RenderDrawRect(renderer_, &box);
    }

    void add_button(SDL_Rect box, const std::string& label, SDL_Color color,
                    std::function<void()> callback, bool enabled = true,
                    bool available_when_locked = false,
                    SDL_Color label_color = kText)
    {
        draw_box(box, enabled ? color : kDark);
        int width = 0;
        int height = 0;
        TTF_SizeUTF8(font_small_, label.c_str(), &width, &height);
        const int x = box.x + std::max(4, (box.w - width) / 2);
        const int y = box.y + std::max(2, (box.h - height) / 2);
        draw_text_clipped(label, x, y, enabled ? label_color : kMuted,
                          {box.x + 4, box.y + 2, std::max(1, box.w - 8),
                           std::max(1, box.h - 4)}, font_small_);
        if (enabled && (!ui_locked_ || available_when_locked)) {
            hits_.push_back({box, std::move(callback)});
        }
    }

    void draw_header()
    {
        draw_box({0, 0, kWidth, 34}, kDark);
        draw_text_clipped("DMR 中继控制台", 12, 8, kText, {12, 5, 108, 24}, font_small_);
        const std::string versions = "R" + state_.repeater_version + " B" +
            std::to_string(state_.repeater_build_sequence) + " G" DMR_B210_GUI_VERSION +
            " B" + std::to_string(DMR_B210_BUILD_SEQUENCE);
        draw_text_clipped(versions, 122, 8, kCyan, {122, 5, 300, 24}, font_small_);
        const std::string online = state_.online ? "在线" : "未在线";
        const SDL_Color online_color = state_.online ? kGreen : kMuted;
        draw_box({540, 13, 9, 9}, online_color);
        draw_text_clipped(online, 555, 8, online_color, {555, 5, 40, 24}, font_small_);
        draw_text_clipped(elapsed_clock(gui_started_), 600, 8, online_color,
                          {600, 5, 58, 24}, font_small_);
        const SDL_Rect lock_box{664, 4, 70, 26};
        draw_box(lock_box, ui_locked_ ? kBlue : kDark);
        draw_text_clipped("全屏锁定", 668, 8, ui_locked_ ? kBackground : kMuted,
                          {668, 5, 62, 24}, font_small_);
        hits_.push_back({lock_box, [this] { ui_locked_ = !ui_locked_; }});
        draw_text_clipped(now_clock(), 741, 8, kText, {741, 5, 52, 24}, font_small_);
    }

    void draw_home()
    {
        draw_box({12, 44, 466, 200});
        draw_text("当前工作信道", 28, 58, kMuted, font_small_);
        const std::string channel_title = state_.active_profile.empty()
            ? "读取中"
            : channel_label(state_.active_profile);
        draw_text_clipped(channel_title, 28, 95, kText, {28, 91, 50, 36}, font_bold_);
        draw_home_frequency("RX", state_.active_channel.rx_frequency_hz,
                            86, 114, 238);
        draw_home_frequency("TX", state_.active_channel.tx_frequency_hz,
                            274, 302, 426);
        draw_box({28, 140, 430, 1}, kLine);
        for (std::size_t index = 0; index < quick_profile_ids_.size() && index < 3U; ++index) {
            const std::string profile_id = quick_profile_ids_[index];
            const int x = 28 + static_cast<int>(index) * 134;
            const bool active = profile_id == state_.active_profile;
            add_button({x, 145, 133, 28}, channel_label(profile_id),
                active ? kCyan : kDark,
                [this, profile_id] { request_switch(profile_id); }, controls_enabled(),
                false, active ? kBackground : kText);
        }
        draw_text_vcentered_clipped("转发控制", 28, kMuted, {28, 175, 229, 19},
                                    font_small_);
        draw_text_vcentered_clipped("增益：" + gain_mode_label(), 334, kMuted,
                                    {334, 175, 130, 19}, font_small_);
        const SDL_Rect forwarding_state_box{28, 198, 229, 35};
        draw_box(forwarding_state_box, state_.rf_fault ? kRed :
                 (state_.forwarding_enabled ? kGreen : kDark));
        const std::string forwarding_state_label = state_.rf_fault ? "射频故障" :
            (state_.forwarding_enabled ? "转发运行中" : "转发待机");
        const std::string forwarding_control_label = state_.rf_fault ? "复位射频" :
            (state_.forwarding_enabled ? "停止转发" : "启动转发");
        const SDL_Color indicator_color = state_.forwarding_enabled ? kRed : kBackground;
        draw_text_vcentered_clipped("●", 44, indicator_color,
                                    {44, forwarding_state_box.y, 18, forwarding_state_box.h}, font_);
        draw_text_vcentered_clipped(
            forwarding_state_label, 66,
            state_.forwarding_enabled && !state_.rf_fault ? kBackground : kText,
            {66, forwarding_state_box.y, 179, forwarding_state_box.h}, font_);
        add_button({267, 198, 126, 35}, forwarding_control_label,
                   state_.rf_fault ? kRed : (state_.forwarding_enabled ? kDark : kGreen), [this] {
            const bool reset_fault = state_.rf_fault;
            send_control(reset_fault || state_.forwarding_enabled ? "stop_forwarding" : "start_forwarding", {},
                         reset_fault ? "复位射频" :
                         (state_.forwarding_enabled ? "停止转发" : "启动转发"));
        }, controls_enabled());

        draw_box({490, 44, 298, 200});
        draw_text("当前接收", 506, 58, kMuted, font_small_);
        if (state_.active_call.valid && !state_.stale) {
            draw_text_clipped(call_mode_text(state_.active_call.mode), 506, 80, kGreen, {506, 78, 152, 25}, font_bold_);
            draw_text(elapsed_clock(active_call_started_), 680, 80, kText, font_);
            draw_text("源 ID  " + std::to_string(state_.active_call.source_id), 506, 110, kText, font_small_);
            draw_text("目标 ID  " + std::to_string(state_.active_call.destination_id), 506, 134, kText, font_small_);
            draw_text("RX" + std::to_string(state_.active_call.rx_channel + 1) + " / " +
                      call_mode_text(state_.active_call.mode), 506, 158, kMuted, font_small_);
        } else {
            draw_text(state_.stale ? "状态陈旧" : "无有效呼叫", 506, 80,
                      state_.stale ? kRed : kMuted, font_bold_);
        }
        draw_box({506, 177, 266, 1}, kLine);
        draw_home_meter();

        draw_box({12, 256, 776, 172});
        draw_box({184, 256, 1, 172}, kLine);
        draw_box({540, 256, 1, 172}, kLine);
        draw_text("接收状态", 28, 270, kMuted, font_small_);
        draw_io_line("RX1", 302, state_.receivers[0].receiving, false,
                     receiver_mode_label(state_.receivers[0].mode));
        draw_io_line("RX2", 330, state_.receivers[1].receiving, false,
                     receiver_mode_label(state_.receivers[1].mode));
        const bool dmr_tx = state_.working_mode == "dmr_relay";
        const bool fm_tx = state_.working_mode == "fm_relay";
        draw_io_line("TX1", 358, dmr_tx, true, dmr_tx ? "转发中" : "待机");
        draw_io_line("TX2", 386, fm_tx, true, fm_tx ? "转发中" : "待机");

        draw_text("运行事件（最近 4 条）", 202, 270, kMuted, font_small_);
        int y = 300;
        for (const EventLine& event : events_) {
            draw_text_clipped(event.time + "  " + event.message, 202, y, event.color,
                              {202, y - 2, 324, 22}, font_small_);
            y += 27;
        }

        draw_text("系统", 556, 270, kMuted, font_small_);
        draw_text("增益", 556, 302, kMuted, font_small_);
        draw_text_clipped(gain_mode_label(), 608, 302, kText, {608, 300, 62, 20}, font_small_);
        draw_text("AFM", 676, 302, kMuted, font_small_);
        draw_text(state_.active_channel.fm_enabled ? "启用" : "关闭", 724, 302,
                  state_.active_channel.fm_enabled ? kGreen : kMuted, font_small_);
        draw_text("存储", 556, 332, kMuted, font_small_);
        const double storage_ratio = state_.recording_storage_limit_bytes == 0
            ? 0.0
            : std::min(1.0, static_cast<double>(state_.recording_storage_bytes) /
                             static_cast<double>(state_.recording_storage_limit_bytes));
        const SDL_Color storage_color = storage_ratio >= 0.90 ? kRed :
            storage_ratio >= 0.75 ? kAmber : kGreen;
        draw_box({598, 334, 58, 14}, kDark);
        if (storage_ratio > 0.0) {
            SDL_Rect fill{599, 335, std::max(1, static_cast<int>(56.0 * storage_ratio)), 12};
            draw_box(fill, storage_color);
        }
        std::ostringstream storage_text;
        storage_text << std::fixed << std::setprecision(1)
                     << (static_cast<double>(state_.recording_storage_bytes) / 1000000.0)
                     << "/" << (static_cast<double>(state_.recording_storage_limit_bytes) / 1000000.0)
                     << " MB";
        draw_text_clipped(storage_text.str(), 662, 331, storage_color,
                          {662, 328, 112, 22}, font_small_);
        draw_text("网络", 556, 382, kMuted, font_small_);
        draw_text(state_.online ? "已连接" : "异常", 608, 382,
                  state_.online ? kGreen : kRed, font_small_);
        if (!state_.last_error.empty()) {
            draw_text_clipped("故障：" + state_.last_error, 556, 406, kRed,
                              {556, 404, 216, 20}, font_small_);
        }
    }

    void draw_io_line(const std::string& label, int y, bool active, bool tx, const std::string& state)
    {
        const SDL_Color color = active ? (tx ? kRed : kGreen) : kMuted;
        draw_text(label, 20, y, color, active && tx ? font_bold_ : font_);
        draw_text_clipped(state, 80, y + 1, color, {80, y, 92, 22},
                          active && tx ? font_bold_ : font_small_);
    }

    std::string receiver_mode_label(const std::string& mode) const
    {
        if (mode == "dmr") return "DMR";
        if (mode == "fm") return "FM";
        return mode;
    }

    std::string gain_mode_label() const
    {
        const std::string selection = state_.gain_selection_mode == "auto"
            ? "自动"
            : "手动";
        if (state_.gain_mode == "high") return selection + "/高";
        if (state_.gain_mode == "medium") return selection + "/中";
        if (state_.gain_mode == "low") return selection + "/低";
        return selection + "/自定义";
    }

    void draw_meter(int x, int y, int receiver)
    {
        const Receiver& item = state_.receivers[static_cast<std::size_t>(receiver)];
        if (!item.rssi_valid) {
            draw_text("RSSI --", x, y, kMuted, font_small_);
            return;
        }
        const bool calibrated = item.calibration_state == "calibrated" &&
            item.rssi_dbm_valid;
        const Meter meter = s_meter(calibrated ? item.rssi_dbm : item.rssi_dbfs,
                                    calibrated
                                        ? kCalibratedS9Dbm
                                        : config_.s9_reference_dbfs[static_cast<std::size_t>(receiver)]);
        draw_text(meter.label, x, y, kText, font_small_);
        for (int index = 0; index < 12; ++index) {
            const SDL_Color lit = index < 8 ? kBlue : index < 10 ? kAmber : kRed;
            draw_box({x + index * 11, y + 22, 8, 12}, index < meter.lit_segments ? lit : kDark);
        }
        std::ostringstream value;
        value << std::fixed << std::setprecision(1)
              << (calibrated ? item.rssi_dbm : item.rssi_dbfs)
              << (calibrated ? " dBm" : " dBFS U");
        draw_text(value.str(), x, y + 39, kMuted, font_small_);
    }

    void draw_home_meter()
    {
        int source = -1;
        bool use_calibrated_dbm = true;
        for (int index = 0; index < static_cast<int>(state_.receivers.size()); ++index) {
            const Receiver& candidate = state_.receivers[static_cast<std::size_t>(index)];
            if (!candidate.rssi_valid) continue;
            if (candidate.mode == "fm" && !state_.active_channel.fm_enabled) continue;
            if (candidate.calibration_state != "calibrated" ||
                !candidate.rssi_dbm_valid) {
                use_calibrated_dbm = false;
            }
        }
        for (int index = 0; index < static_cast<int>(state_.receivers.size()); ++index) {
            const Receiver& candidate = state_.receivers[static_cast<std::size_t>(index)];
            if (!candidate.rssi_valid) continue;
            if (candidate.mode == "fm" && !state_.active_channel.fm_enabled) continue;
            if (source < 0 || (use_calibrated_dbm ? candidate.rssi_dbm : candidate.rssi_dbfs) >
                (use_calibrated_dbm
                    ? state_.receivers[static_cast<std::size_t>(source)].rssi_dbm
                    : state_.receivers[static_cast<std::size_t>(source)].rssi_dbfs)) {
                source = index;
            }
        }
        draw_text("信号强度", 506, 185, kMuted, font_small_);
        if (source < 0) {
            draw_text("--", 736, 185, kMuted, font_small_);
            return;
        }
        const Receiver& item = state_.receivers[static_cast<std::size_t>(source)];
        const Meter meter = s_meter(use_calibrated_dbm ? item.rssi_dbm : item.rssi_dbfs,
                                    use_calibrated_dbm
                                        ? kCalibratedS9Dbm
                                        : config_.s9_reference_dbfs[static_cast<std::size_t>(source)]);
        draw_text_clipped(meter.label, 682, 185, kText, {682, 182, 86, 24}, font_small_);
        for (int index = 0; index < 12; ++index) {
            const int x = 506 + index * 22;
            const SDL_Color color = index < 8 ? kBlue : index < 10 ? kAmber : kRed;
            draw_box({x, 210, 19, 14}, index < meter.lit_segments ? color : kDark);
            std::string label;
            if (index < 9) label = "S" + std::to_string(index + 1);
            else label = "+" + std::to_string((index - 8) * 20);
            draw_text_clipped(label, x, 228, index < meter.lit_segments ? color : kMuted,
                              {x, 226, 21, 18}, font_small_);
        }
    }

    void draw_channel_page()
    {
        draw_box({12, 46, 776, 382});
        draw_text("信道总览", 24, 60, kText, font_bold_);
        draw_text("已配置 " + std::to_string(config_.profile_ids.size()) + " 个信道", 24, 88, kMuted, font_small_);
        for (std::size_t index = 0; index < config_.profile_ids.size() && index < 8U; ++index) {
            const std::string& profile_id = config_.profile_ids[index];
            const int column = static_cast<int>(index / 4U);
            const int row = static_cast<int>(index % 4U);
            const int x = column == 0 ? 26 : 410;
            const int y = 116 + row * 66;
            const bool active = profile_id == state_.active_profile;
            const bool quick = is_quick_profile(profile_id);
            add_button({x, y + 11, 32, 32}, quick ? "✓" : "",
                       quick ? kGreen : kDark,
                       [this, profile_id] { toggle_quick_profile(profile_id); },
                       controls_enabled());
            add_button({x + 42, y, 314, 54}, "", active ? kCyan : kPanel,
                       [this, profile_id] { open_channel_details(profile_id); }, controls_enabled());
            draw_text(channel_label(profile_id), x + 56, y + 10, active ? kBackground : kText, font_bold_);
            const auto found = channels_.find(profile_id);
            if (found == channels_.end()) {
                draw_text("RX / TX 读取中", x + 126, y + 18, kMuted, font_small_);
            } else {
                draw_text_clipped("RX " + frequency_text(found->second.rx_frequency_hz) + "  |  TX " +
                                   frequency_text(found->second.tx_frequency_hz),
                                  x + 126, y + 18, active ? kBackground : kMuted,
                                  {x + 126, y + 14, 216, 28}, font_small_);
            }
            if (active) draw_text("当前", x + 56, y + 34, kBackground, font_small_);
        }
    }

    void request_switch(const std::string& id, bool persist_active_profile = true,
                        const std::string& description = {})
    {
        selected_profile_ = id;
        const std::string action = description.empty()
            ? (persist_active_profile ? "激活并保存" : "切换信道")
            : description;
        if (!controls_enabled()) return;
        try {
            const std::string request_id = client_.send(
                "switch_channel", "\"profile_id\":\"" + json_escape(id) +
                "\",\"persist_active_profile\":" +
                (persist_active_profile ? "true" : "false"));
            track(request_id, action, "switch_channel");
            switch_requests_[request_id] = id;
            latest_switch_request_id_ = request_id;
            pending_switch_profile_ = id;
            pending_switch_action_ = action;
            pending_switch_accepted_ = false;
            pending_switch_state_confirmed_ = false;
            pending_switch_started_ = std::chrono::steady_clock::now();
            add_event(action + "：等待确认", kAmber);
        } catch (const std::exception& error) {
            add_event(std::string("UDP 发送失败：") + error.what(), kRed);
        }
    }

    std::string channel_label(const std::string& profile_id) const
    {
        const auto found = std::find(config_.profile_ids.begin(), config_.profile_ids.end(), profile_id);
        if (found == config_.profile_ids.end()) return profile_id;
        return "CH" + std::to_string(std::distance(config_.profile_ids.begin(), found) + 1);
    }

    bool is_quick_profile(const std::string& profile_id) const
    {
        return std::find(quick_profile_ids_.begin(), quick_profile_ids_.end(), profile_id) !=
            quick_profile_ids_.end();
    }

    void toggle_quick_profile(const std::string& profile_id)
    {
        const std::vector<std::string> previous = quick_profile_ids_;
        const auto found = std::find(quick_profile_ids_.begin(), quick_profile_ids_.end(), profile_id);
        const bool was_quick = found != quick_profile_ids_.end();
        if (was_quick) {
            quick_profile_ids_.erase(found);
        } else if (quick_profile_ids_.size() >= 3U) {
            add_event("快速信道最多选择 3 个", kRed);
            return;
        } else {
            quick_profile_ids_.push_back(profile_id);
        }
        sort_channel_profiles(quick_profile_ids_);
        try {
            persist_gui_quick_profiles(config_.config_path, quick_profile_ids_);
            add_event(channel_label(profile_id) +
                          (was_quick
                               ? " 已移出快速信道" : " 已加入快速信道"),
                      kGreen);
        } catch (const std::exception& error) {
            quick_profile_ids_ = previous;
            add_event("快捷信道保存失败：" + std::string(error.what()), kRed);
        }
    }

    Channel selected_channel() const
    {
        const std::string& profile_id = detail_profile_id_.empty() ? state_.active_profile : detail_profile_id_;
        if (profile_id == draft_profile_id_ && draft_channel_.id == profile_id) {
            return draft_channel_;
        }
        const auto found = channels_.find(profile_id);
        return found == channels_.end() ? state_.active_channel : found->second;
    }

    void begin_channel_draft(const std::string& profile_id)
    {
        if (profile_id.empty() || draft_profile_id_ == profile_id) return;
        draft_profile_id_ = profile_id;
        draft_dirty_ = false;
        const auto found = channels_.find(profile_id);
        if (found != channels_.end()) {
            draft_channel_ = found->second;
        } else if (state_.active_channel.id == profile_id) {
            draft_channel_ = state_.active_channel;
        } else {
            draft_channel_ = {};
            draft_channel_.id = profile_id;
        }
    }

    void request_channel_refresh(const std::string& profile_id)
    {
        if (!state_.online) return;
        const bool already_requested = std::any_of(channel_requests_.begin(), channel_requests_.end(),
            [&](const auto& item) { return item.second == profile_id; });
        if (already_requested) return;
        try {
            const std::string request_id = client_.send("get_channel",
                "\"profile_id\":\"" + json_escape(profile_id) + "\"");
            track(request_id, "读取 " + channel_label(profile_id));
            channel_requests_[request_id] = profile_id;
        } catch (const std::exception& error) {
            add_event(std::string("读取信道失败：") + error.what(), kRed);
        }
    }

    void refresh_due_channels()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<std::string> due_profiles;
        for (auto it = channel_refresh_due_.begin(); it != channel_refresh_due_.end();) {
            if (it->second <= now) {
                due_profiles.push_back(it->first);
                it = channel_refresh_due_.erase(it);
            } else {
                ++it;
            }
        }
        for (const std::string& profile_id : due_profiles) request_channel_refresh(profile_id);
    }

    void request_channel_summaries()
    {
        if (!state_.online) return;
        for (const std::string& profile_id : config_.profile_ids) {
            if (channels_.find(profile_id) != channels_.end()) continue;
            request_channel_refresh(profile_id);
        }
    }

    void open_channel_details(const std::string& profile_id)
    {
        detail_profile_id_ = profile_id;
        begin_channel_draft(profile_id);
        page_ = 2;
        if (channels_.find(profile_id) == channels_.end()) request_channel_summaries();
    }

    void send_channel_patch(const std::string& profile_id, const std::string& key,
                            std::int64_t value, const std::string& description)
    {
        if (!controls_enabled()) return;
        try {
            const std::string request_id = client_.send("set_channel",
                "\"profile_id\":\"" + json_escape(profile_id) + "\",\"" + key + "\":" +
                std::to_string(value));
            track(request_id, description, "set_channel");
            channel_requests_[request_id] = profile_id;
            channel_refresh_due_[profile_id] = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(800);
            add_event(description + "：等待确认", kAmber);
        } catch (const std::exception& error) {
            add_event(std::string("UDP 发送失败：") + error.what(), kRed);
        }
    }

    void send_channel_patch(const std::string& profile_id, const std::string& key,
                            bool value, const std::string& description)
    {
        if (!controls_enabled()) return;
        try {
            const std::string request_id = client_.send("set_channel",
                "\"profile_id\":\"" + json_escape(profile_id) + "\",\"" + key + "\":" +
                (value ? "true" : "false"));
            track(request_id, description, "set_channel");
            channel_requests_[request_id] = profile_id;
            if (key == "fm_enabled") {
                const auto channel = channels_.find(profile_id);
                if (channel != channels_.end()) channel->second.fm_enabled = value;
                if (state_.active_channel.id == profile_id) state_.active_channel.fm_enabled = value;
            }
            channel_refresh_due_[profile_id] = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(800);
            add_event(description + "：等待确认", kAmber);
        } catch (const std::exception& error) {
            add_event(std::string("UDP 发送失败：") + error.what(), kRed);
        }
    }

    void draw_parameter_page()
    {
        draw_box({12, 46, 776, 382});
        const std::string profile_id = detail_profile_id_.empty() ? state_.active_profile : detail_profile_id_;
        begin_channel_draft(profile_id);
        const Channel channel = selected_channel();
        const bool editing_active = profile_id == state_.active_profile;
        const int ctcss_tone_tenths_hz = standard_ctcss_tone(channel.ctcss_tone_tenths_hz);
        std::ostringstream ctcss;
        ctcss << std::fixed << std::setprecision(1)
              << static_cast<double>(ctcss_tone_tenths_hz) / 10.0 << " Hz";
        draw_text(channel_label(profile_id) + " 信道详细设置", 24, 60, kText, font_bold_);
        draw_text_clipped(profile_id, 24, 88, kMuted, {24, 86, 250, 20}, font_small_);
        draw_adjuster("DMR RX", frequency_text(channel.rx_frequency_hz), 108,
            [this, profile_id, channel] { adjust_channel(profile_id, "rx_frequency_hz", channel.rx_frequency_hz - 12500); },
            [this, profile_id, channel] { adjust_channel(profile_id, "rx_frequency_hz", channel.rx_frequency_hz + 12500); });
        draw_adjuster("DMR TX", frequency_text(channel.tx_frequency_hz), 146,
            [this, profile_id, channel] { adjust_channel(profile_id, "tx_frequency_hz", channel.tx_frequency_hz - 12500); },
            [this, profile_id, channel] { adjust_channel(profile_id, "tx_frequency_hz", channel.tx_frequency_hz + 12500); });
        draw_adjuster("RX 增益", gain_text(channel.rx_gain_tenths_db), 184,
            [this, profile_id, channel] { adjust_channel(profile_id, "rx_gain_tenths_db", channel.rx_gain_tenths_db - 10); },
            [this, profile_id, channel] { adjust_channel(profile_id, "rx_gain_tenths_db", channel.rx_gain_tenths_db + 10); });
        draw_adjuster("TX 增益", gain_text(channel.tx_gain_tenths_db), 222,
            [this, profile_id, channel] { adjust_channel(profile_id, "tx_gain_tenths_db", channel.tx_gain_tenths_db - 10); },
            [this, profile_id, channel] { adjust_channel(profile_id, "tx_gain_tenths_db", channel.tx_gain_tenths_db + 10); });
        draw_adjuster("CTCSS", ctcss.str(), 260,
            [this, profile_id, channel] {
                adjust_channel(profile_id, "ctcss_tone_tenths_hz",
                               adjust_standard_ctcss_tone(channel.ctcss_tone_tenths_hz, -1));
            },
            [this, profile_id, channel] {
                adjust_channel(profile_id, "ctcss_tone_tenths_hz",
                               adjust_standard_ctcss_tone(channel.ctcss_tone_tenths_hz, 1));
            });
        add_button({30, 306, 130, 34}, channel.fm_enabled ? "FM 已启用" : "FM 已关闭",
                   channel.fm_enabled ? kAmber : kDark, [this, profile_id, channel] {
            stage_channel_value(profile_id, "fm_enabled", !channel.fm_enabled);
        }, controls_enabled());
        add_button({174, 306, 104, 34}, "返回信道", kDark, [this] { page_ = 1; });
        add_button({292, 306, 110, 34}, draft_dirty_ ? "保存*" : "保存", kGreen,
                   [this, profile_id] { save_channel_draft(profile_id); }, controls_enabled());
        add_button({416, 306, 138, 34}, "激活此信道", kDark,
                   [this, profile_id] { request_switch(profile_id, true); }, controls_enabled());
        add_button({568, 306, 188, 34}, "设为开机信道", kDark,
                   [this, profile_id] {
                       request_switch(profile_id, true, "设置开机信道");
                   }, controls_enabled());
        add_button({64, 366, 150, 34}, "自动", state_.gain_selection_mode == "auto" ? kCyan : kDark,
                   [this] { send_control("set_gain_mode", "\"gain_mode\":\"auto\"", "启用自动增益"); },
                   controls_enabled() && editing_active);
        add_button({224, 366, 150, 34}, "高档", state_.gain_selection_mode == "manual" && state_.gain_mode == "high" ? kCyan : kDark,
                   [this] { send_control("set_gain_mode", "\"gain_mode\":\"high\"", "切换高增益"); },
                   controls_enabled() && editing_active);
        add_button({384, 366, 150, 34}, "中档", state_.gain_selection_mode == "manual" && state_.gain_mode == "medium" ? kCyan : kDark,
                   [this] { send_control("set_gain_mode", "\"gain_mode\":\"medium\"", "切换中增益"); },
                   controls_enabled() && editing_active);
        add_button({544, 366, 150, 34}, "低档", state_.gain_selection_mode == "manual" && state_.gain_mode == "low" ? kCyan : kDark,
                   [this] { send_control("set_gain_mode", "\"gain_mode\":\"low\"", "切换低增益"); },
                   controls_enabled() && editing_active);
    }

    void draw_adjuster(const std::string& label, const std::string& value, int y,
                       std::function<void()> down, std::function<void()> up)
    {
        draw_text(label, 30, y + 7, kText, font_);
        draw_box({150, y, 320, 32});
        draw_text_clipped(value, 164, y + 8, kCyan, {164, y + 4, 292, 24}, font_);
        add_button({492, y, 72, 32}, "-", kDark, std::move(down), controls_enabled());
        add_button({576, y, 72, 32}, "+", kDark, std::move(up), controls_enabled());
    }

    void adjust_channel(const std::string& profile_id, const std::string& key, std::int64_t value)
    {
        if ((key.find("frequency") != std::string::npos && (value < 136000000 || value > 520000000)) ||
            (key == "ctcss_tone_tenths_hz" && (value < 1 || value > 3000))) {
            add_event("参数超出界面允许范围", kRed);
            return;
        }
        stage_channel_value(profile_id, key, value);
    }

    void step_rx_calibration()
    {
        if (!calibration_step_available(calibration_)) return;
        send_control("rx_calibration_step",
                     "\"session_id\":\"" + calibration_.session_id +
                         "\",\"input_dbm\":" +
                         std::to_string(*calibration_.next_input_dbm),
                     "CAL submit");
    }

    void save_rx_calibration()
    {
        if (calibration_.session_id.empty()) return;
        send_control("rx_calibration_commit",
                     "\"session_id\":\"" + calibration_.session_id + "\"",
                     "CAL save");
    }

    void cancel_rx_calibration()
    {
        if (calibration_.session_id.empty()) return;
        send_control("rx_calibration_cancel",
                     "\"session_id\":\"" + calibration_.session_id + "\"",
                     "CAL cancel");
    }

    void open_calibration_page()
    {
        page_ = 4;
        calibration_leave_dialog_ = false;
        calibration_exit_after_save_ = false;
        calibration_exit_after_discard_ = false;
        calibration_.authorized = false;
        calibration_.password_input.clear();
        calibration_.password_error.clear();
    }

    bool calibration_has_unsaved_work() const
    {
        if (!calibration_.session_id.empty()) return true;
        return std::any_of(pending_.begin(), pending_.end(), [](const auto& item) {
            return item.second == "CAL begin" || item.second == "CAL submit" ||
                item.second == "CAL save" || item.second == "CAL cancel";
        });
    }

    void finish_calibration_exit()
    {
        calibration_leave_dialog_ = false;
        calibration_exit_after_save_ = false;
        calibration_exit_after_discard_ = false;
        calibration_.authorized = false;
        calibration_.password_input.clear();
        calibration_.password_error.clear();
        page_ = calibration_exit_target_page_;
    }

    void request_calibration_exit(int target_page = 3)
    {
        calibration_exit_target_page_ = target_page;
        if (!calibration_has_unsaved_work()) {
            finish_calibration_exit();
            return;
        }
        calibration_leave_dialog_ = true;
    }

    void discard_calibration_and_exit()
    {
        if (!calibration_has_unsaved_work()) {
            finish_calibration_exit();
            return;
        }
        calibration_leave_dialog_ = false;
        calibration_exit_after_discard_ = true;
        cancel_rx_calibration();
    }

    void save_calibration_and_exit()
    {
        if (calibration_.completed_points != 9 || calibration_.session_id.empty()) {
            return;
        }
        calibration_leave_dialog_ = false;
        calibration_exit_after_save_ = true;
        save_rx_calibration();
    }

    void request_calibration_refresh()
    {
        if (calibration_query_request_) return;
        try {
            calibration_query_request_ = client_.send("get_rx_calibration");
            calibration_query_sent_at_ = std::chrono::steady_clock::now();
        } catch (...) {
        }
    }

    void refresh_calibration_page()
    {
        if (qa_view_enabled_ || page_ != 4 || !calibration_.authorized) return;
        const auto now = std::chrono::steady_clock::now();
        if (calibration_query_request_ &&
            now - calibration_query_sent_at_ > std::chrono::seconds(1)) {
            calibration_query_request_.reset();
        }
        if (now < next_calibration_query_) return;
        request_calibration_refresh();
        next_calibration_query_ = now + std::chrono::milliseconds(200);
    }

    void refresh_pending_calibration_exit()
    {
        if (!calibration_exit_after_save_ && !calibration_exit_after_discard_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_calibration_query_) {
            // A lost UDP reply must not leave save/discard exit waiting forever.
            calibration_query_request_.reset();
            request_calibration_refresh();
            next_calibration_query_ = now + std::chrono::milliseconds(150);
        }
    }

    void select_calibration_column(int column)
    {
        if (column < 0 || column >= 6) return;
        if (!calibration_.session_id.empty()) {
            add_event("校准进行中，档位已锁定", kAmber);
            return;
        }
        calibration_.selected_column = column;
        calibration_.rx_channel = column / 3;
        const int band = column % 3;
        calibration_.band = band == 0 ? "low" : band == 1 ? "medium" : "high";
        calibration_.gain_tenths_db = selected_calibration_gain(calibration_);
    }

    void set_calibration_gain(int delta_tenths_db)
    {
        if (calibration_.band == "low" || !calibration_.session_id.empty()) {
            calibration_.gain_tenths_db = 0;
            return;
        }
        const int column = calibration_.selected_column;
        const int current = selected_calibration_gain(calibration_);
        const int next = std::clamp(current + delta_tenths_db, 10, 1000);
        if (next != current) {
            calibration_.column_points[static_cast<std::size_t>(column)].clear();
            calibration_.column_gain[static_cast<std::size_t>(column)] = next;
            calibration_.column_edited[static_cast<std::size_t>(column)] = true;
        }
        calibration_.gain_tenths_db = next;
    }

    void auto_calibration_gain()
    {
        if (calibration_.band == "low" || !calibration_.session_id.empty()) {
            calibration_.gain_tenths_db = 0;
            return;
        }
        const Receiver& receiver = state_.receivers[static_cast<std::size_t>(calibration_.rx_channel)];
        if (!receiver.rssi_valid) {
            add_event("CAL auto: RSSI unavailable", kRed);
            return;
        }
        const int current = selected_calibration_gain(calibration_);
        const int next = std::clamp(
            current + static_cast<int>(std::lround(
                (-20.0 - receiver.rssi_dbfs) * 10.0)), 10, 1000);
        if (next != current) {
            calibration_.column_points[static_cast<std::size_t>(calibration_.selected_column)].clear();
            calibration_.column_gain[static_cast<std::size_t>(calibration_.selected_column)] = next;
            calibration_.column_edited[
                static_cast<std::size_t>(calibration_.selected_column)] = true;
        }
        calibration_.gain_tenths_db = next;
    }

    void submit_calibration_password()
    {
        if (calibration_.password_input == config_.calibration_password) {
            calibration_.authorized = true;
            calibration_.password_error.clear();
            request_calibration_refresh();
            return;
        }
        calibration_.password_input.clear();
        calibration_.password_error = "密码错误";
    }

    void append_calibration_digit(char digit)
    {
        if (calibration_.password_input.size() < 8U) {
            calibration_.password_input.push_back(digit);
        }
    }

    void begin_selected_calibration()
    {
        const int gain_tenths_db = selected_calibration_gain(calibration_);
        calibration_.gain_tenths_db = gain_tenths_db;
        if (calibration_.band != "low") {
            calibration_.column_gain[
                static_cast<std::size_t>(calibration_.selected_column)] =
                gain_tenths_db;
            calibration_.column_edited[
                static_cast<std::size_t>(calibration_.selected_column)] = true;
        }
        std::string fields = "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
            ",\"range\":\"" + calibration_.band + "\"" +
            ",\"rx_gain_tenths_db\":" + std::to_string(
                gain_tenths_db);
        send_control("rx_calibration_begin", fields, "CAL begin");
    }

    void stage_channel_value(const std::string& profile_id, const std::string& key,
                             std::int64_t value)
    {
        begin_channel_draft(profile_id);
        if (key == "rx_frequency_hz") draft_channel_.rx_frequency_hz = value;
        else if (key == "tx_frequency_hz") draft_channel_.tx_frequency_hz = value;
        else if (key == "rx_gain_tenths_db") draft_channel_.rx_gain_tenths_db = static_cast<int>(value);
        else if (key == "tx_gain_tenths_db") draft_channel_.tx_gain_tenths_db = static_cast<int>(value);
        else if (key == "ctcss_tone_tenths_hz") draft_channel_.ctcss_tone_tenths_hz = static_cast<int>(value);
        else return;
        draft_dirty_ = true;
        add_event("已修改 " + channel_label(profile_id) + "，请保存", kAmber);
    }

    void stage_channel_value(const std::string& profile_id, const std::string& key,
                             bool value)
    {
        begin_channel_draft(profile_id);
        if (key != "fm_enabled") return;
        draft_channel_.fm_enabled = value;
        draft_dirty_ = true;
        add_event("已修改 " + channel_label(profile_id) + "，请保存", kAmber);
    }

    void save_channel_draft(const std::string& profile_id)
    {
        if (!controls_enabled() || profile_id != draft_profile_id_ || !draft_dirty_) return;
        try {
            draft_channel_.ctcss_tone_tenths_hz =
                standard_ctcss_tone(draft_channel_.ctcss_tone_tenths_hz);
            const std::string request_id = client_.send("save_channel",
                "\"profile_id\":\"" + json_escape(profile_id) + "\",\"rx_frequency_hz\":" +
                std::to_string(draft_channel_.rx_frequency_hz) + ",\"tx_frequency_hz\":" +
                std::to_string(draft_channel_.tx_frequency_hz) + ",\"rx_gain_tenths_db\":" +
                std::to_string(draft_channel_.rx_gain_tenths_db) + ",\"tx_gain_tenths_db\":" +
                std::to_string(draft_channel_.tx_gain_tenths_db) + ",\"fm_enabled\":" +
                (draft_channel_.fm_enabled ? "true" : "false") + ",\"ctcss_tone_tenths_hz\":" +
                std::to_string(draft_channel_.ctcss_tone_tenths_hz));
            track(request_id, "保存 " + channel_label(profile_id), "save_channel");
            channel_requests_[request_id] = profile_id;
            channel_refresh_due_[profile_id] = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(800);
            draft_dirty_ = false;
            add_event("保存 " + channel_label(profile_id) + "：等待确认", kAmber);
        } catch (const std::exception& error) {
            add_event(std::string("保存失败：") + error.what(), kRed);
        }
    }

    void draw_status_page()
    {
        draw_box({12, 46, 776, 382});
        draw_text("实时状态", 24, 60, kText, font_bold_);
        draw_text(state_.stale ? "订阅陈旧（超过 1 秒）" : "订阅健康（200 ms）",
                  24, 84, state_.stale ? kRed : kGreen, font_);
        draw_text_clipped("当前接收 " + frequency_text(state_.active_channel.rx_frequency_hz) +
                          "    当前发射 " + frequency_text(state_.active_channel.tx_frequency_hz),
                          24, 106, kCyan, {24, 102, 752, 24}, font_small_);
        for (int index = 0; index < 2; ++index) {
            const Receiver& receiver = state_.receivers[static_cast<std::size_t>(index)];
            const int y = 132 + index * 100;
            draw_box({24, y, 468, 94});
            draw_text("RX" + std::to_string(index + 1) + " " + receiver_mode_label(receiver.mode),
                      38, y + 7, receiver.receiving ? kGreen : kMuted, font_);
            if (receiver.rssi_valid) {
                std::ostringstream text;
                const bool calibrated = receiver.calibration_state == "calibrated" &&
                    receiver.rssi_dbm_valid;
                text << std::fixed << std::setprecision(1) << "RSSI "
                     << (calibrated ? receiver.rssi_dbm : receiver.rssi_dbfs)
                     << (calibrated ? " dBm" : " dBFS U")
                     << "  SNR " << (receiver.snr_valid ? receiver.snr_db : 0.0)
                     << " dB";
                draw_text_clipped(text.str(), 38, y + 30, kText,
                                  {38, y + 27, 282, 22}, font_small_);
            } else {
                draw_text("RSSI/SNR 未知", 38, y + 30, kMuted, font_small_);
            }
            const std::string hardware_gain = receiver.analog_gain_valid
                ? gain_text(static_cast<int>(std::lround(receiver.analog_gain_db * 10.0)))
                : "--";
            const std::string software_gain = receiver.software_agc_gain_valid
                ? gain_text(static_cast<int>(std::lround(receiver.software_agc_gain_db * 10.0)))
                : "--";
            std::ostringstream agc_input;
            if (receiver.agc_input_valid) {
                agc_input << std::fixed << std::setprecision(1)
                          << receiver.agc_input_dbfs << " dBFS";
            } else {
                agc_input << "--";
            }
            const std::string compensation = receiver.rssi_gain_compensation_valid
                ? gain_text(static_cast<int>(std::lround(
                    receiver.rssi_gain_compensation_db * 10.0)))
                : "--";
            draw_text_clipped("硬件AGC " + std::string(receiver.hardware_agc_enabled ? "开" : "关") +
                              "  硬件增益 " + hardware_gain,
                              38, y + 52, receiver.hardware_agc_enabled ? kGreen : kAmber,
                              {38, y + 49, 282, 20}, font_small_);
            draw_text_clipped("软件AGC " + software_gain + "  输入 " + agc_input.str() +
                              "  补偿 " + compensation,
                              38, y + 72, kMuted, {38, y + 69, 430, 20}, font_small_);
            draw_meter(334, y + 7, index);
        }
        draw_box({510, 132, 266, 194});
        draw_text("呼叫与故障", 524, 145, kMuted, font_small_);
        draw_text(state_.active_call.valid ? call_mode_text(state_.active_call.mode) : "无有效呼叫",
                  524, 172, state_.active_call.valid ? kGreen : kMuted, font_);
        if (state_.active_call.valid) {
            draw_text_clipped("源 " + std::to_string(state_.active_call.source_id) + "  目标 " +
                              std::to_string(state_.active_call.destination_id),
                              524, 200, kText, {524, 198, 236, 22}, font_small_);
        }
        draw_text_clipped("工作方式：" + state_.working_mode, 524, 232, kCyan,
                          {524, 230, 236, 22}, font_small_);
        draw_text_clipped(state_.last_error.empty() ? "最近错误：无" : "最近错误：" + state_.last_error,
                          524, 264, state_.last_error.empty() ? kGreen : kRed,
                          {524, 262, 236, 44}, font_small_);
        add_button({510, 338, 266, 38}, "RSSI校准", kDark,
                   [this] { open_calibration_page(); }, controls_enabled());
    }

    void draw_calibration_password_page()
    {
        draw_box({12, 46, 776, 382});
        draw_text("RSSI校准", 28, 62, kText, font_bold_);
        add_button({646, 54, 126, 28}, "返回状态", kDark,
                   [this] { request_calibration_exit(3); });
        draw_text("请输入8位校准密码", 28, 96, kMuted, font_);
        draw_box({28, 124, 310, 38}, kDark);
        const std::string masked(calibration_.password_input.size(), '*');
        draw_text(masked, 44, 133, kCyan, font_bold_);
        if (!calibration_.password_error.empty()) {
            draw_text(calibration_.password_error, 28, 174, kRed, font_small_);
        }
        const std::array<std::string, 12> keys{
            "1", "2", "3", "4", "5", "6", "7", "8", "9", "清除", "0", "确定"};
        for (int index = 0; index < 12; ++index) {
            const int column = index % 3;
            const int row = index / 3;
            const SDL_Rect box{392 + column * 104, 86 + row * 58, 92, 48};
            add_button(box, keys[static_cast<std::size_t>(index)],
                       index == 11 ? kGreen : kDark,
                       [this, index] {
                           if (index == 9) calibration_.password_input.clear();
                           else if (index == 11) submit_calibration_password();
                           else append_calibration_digit(static_cast<char>('0' +
                               (index == 10 ? 0 : index + 1)));
                       }, controls_enabled());
        }
    }

    template <std::size_t N>
    void draw_calibration_table(const std::string& band,
                                const std::array<int, N>& inputs, int x)
    {
        constexpr int table_y = 188;
        constexpr int table_width = 744;
        constexpr int table_height = 216;
        constexpr int first_value_x_offset = 110;
        constexpr int value_width = 300;
        const int band_index = calibration_band_index(band);
        const bool session_active = calibration_step_available(calibration_) &&
            calibration_.band == band;

        draw_box({x, table_y, table_width, table_height}, kPanel);
        const std::string range_name = band == "low" ? "低档校准" :
            band == "medium" ? "中档校准" : "高档校准";
        draw_text_vcentered_clipped(range_name, x + 10,
                                    band == "high" ? kAmber : kCyan,
                                    {x + 10, table_y + 5, 132, 20}, font_);
        draw_text_vcentered_clipped(
            std::to_string(inputs.front()) + " 至 " +
                std::to_string(inputs.back()) + " dBm",
            x + 150, kMuted,
            {x + 150, table_y + 7, 220, 18}, font_small_);
        draw_text_vcentered_clipped("dBm", x + 12, kMuted,
                                    {x + 12, table_y + 29, 46, 18}, font_small_);

        for (int rx = 0; rx < 2; ++rx) {
            const int column = rx * 3 + band_index;
            const int value_x = x + first_value_x_offset + rx * value_width;
            const int gain = band != "low"
                ? calibration_.column_gain[static_cast<std::size_t>(column)].value_or(0)
                : 0;
            draw_text_vcentered_clipped(
                "RX" + std::to_string(rx + 1) + " " + gain_text(gain), value_x,
                kMuted, {value_x, table_y + 29, value_width - 4, 18}, font_small_);
        }

        for (std::size_t row = 0; row < inputs.size(); ++row) {
            const int input = inputs[row];
            const int row_y = table_y + 50 + static_cast<int>(row) * 14;
            draw_text_vcentered_clipped(std::to_string(input), x + 12, kText,
                                        {x + 12, row_y, 46, 13}, font_small_);
            for (int rx = 0; rx < 2; ++rx) {
                const int column = rx * 3 + band_index;
                const int value_x = x + first_value_x_offset + rx * value_width;
                const bool current_point = session_active &&
                    calibration_.rx_channel == rx &&
                    *calibration_.next_input_dbm == input;
                draw_box({value_x, row_y, value_width - 4, 13},
                         current_point ? kCyan : kDark);
                const auto found = calibration_.column_points[
                    static_cast<std::size_t>(column)].find(input);
                std::string value = current_point ? "待测" : "--";
                if (!current_point && found != calibration_.column_points[
                                          static_cast<std::size_t>(column)].end()) {
                    std::ostringstream measured;
                    measured << std::fixed << std::setprecision(1) << found->second.first;
                    value = measured.str();
                }
                draw_text_vcentered_clipped(value, value_x + 5,
                                            current_point ? kBackground :
                                            found == calibration_.column_points[
                                                static_cast<std::size_t>(column)].end()
                                                ? kMuted : kCyan,
                                            {value_x + 5, row_y, value_width - 12, 13},
                                            font_small_);
            }
        }
    }

    void draw_calibration_page()
    {
        if (!calibration_.authorized) {
            draw_calibration_password_page();
            return;
        }
        draw_box({12, 46, 776, 382});
        draw_text("RSSI校准", 24, 58, kText, font_bold_);
        add_button({646, 54, 126, 28}, "返回状态", kDark,
                   [this] { request_calibration_exit(3); }, controls_enabled());
        draw_text("当前输入", 24, 86, kMuted, font_small_);
        const Receiver& receiver = state_.receivers[static_cast<std::size_t>(calibration_.rx_channel)];
        std::ostringstream live;
        live << "RX" << (calibration_.rx_channel + 1) << "  " << std::fixed
             << std::setprecision(1)
             << (receiver.rssi_valid ? receiver.rssi_dbfs : 0.0) << " dBFS  SNR "
             << (receiver.snr_valid ? receiver.snr_db : 0.0) << " dB";
        draw_text_clipped(receiver.rssi_valid ? live.str() : "RX --", 104, 86, kCyan,
                          {104, 82, 258, 24}, font_small_);
        const std::string analog_gain = receiver.analog_gain_valid
            ? gain_text(static_cast<int>(std::lround(receiver.analog_gain_db * 10.0)))
            : "--";
        const std::string software_gain = receiver.software_agc_gain_valid
            ? gain_text(static_cast<int>(std::lround(receiver.software_agc_gain_db * 10.0)))
            : "--";
        std::ostringstream agc_input;
        if (receiver.agc_input_valid) {
            agc_input << std::fixed << std::setprecision(1)
                      << receiver.agc_input_dbfs << " dBFS";
        } else {
            agc_input << "--";
        }
        const std::string compensation = receiver.rssi_gain_compensation_valid
            ? gain_text(static_cast<int>(std::lround(
                receiver.rssi_gain_compensation_db * 10.0)))
            : "--";
        draw_text_clipped("硬件AGC " + std::string(receiver.hardware_agc_enabled ? "开" : "关"),
                          370, 82, receiver.hardware_agc_enabled ? kGreen : kAmber,
                          {370, 80, 96, 22}, font_small_);
        draw_text_clipped("硬件增益 " + analog_gain, 470, 82, kCyan,
                          {470, 80, 174, 22}, font_small_);
        draw_text_clipped("软件AGC增益 " + software_gain, 370, 102, kMuted,
                          {370, 100, 174, 20}, font_small_);
        draw_text_clipped("输入 " + agc_input.str(), 548, 102, kMuted,
                          {548, 100, 116, 20}, font_small_);
        draw_text_clipped("补偿 " + compensation, 666, 102, kCyan,
                          {666, 100, 100, 20}, font_small_);
        const bool calibration_selection_enabled = controls_enabled() &&
            calibration_.session_id.empty();
        add_button({24, 122, 76, 28}, "RX1", calibration_.rx_channel == 0 ? kCyan : kDark,
                    [this] { select_calibration_column(calibration_band_index(calibration_.band)); }, calibration_selection_enabled);
        add_button({108, 122, 76, 28}, "RX2", calibration_.rx_channel == 1 ? kCyan : kDark,
                    [this] { select_calibration_column(3 + calibration_band_index(calibration_.band)); }, calibration_selection_enabled);
        add_button({196, 122, 60, 28}, "低档", calibration_.band == "low" ? kCyan : kDark,
                    [this] { select_calibration_column(calibration_.rx_channel * 3); }, calibration_selection_enabled);
        add_button({264, 122, 60, 28}, "中档", calibration_.band == "medium" ? kCyan : kDark,
                    [this] { select_calibration_column(calibration_.rx_channel * 3 + 1); }, calibration_selection_enabled);
        add_button({332, 122, 60, 28}, "高档", calibration_.band == "high" ? kCyan : kDark,
                    [this] { select_calibration_column(calibration_.rx_channel * 3 + 2); }, calibration_selection_enabled);
        const int gain = selected_calibration_gain(calibration_);
        draw_text_vcentered_clipped("增益 " + gain_text(gain), 400, kCyan,
                                    {400, 122, 104, 28}, font_small_);
        add_button({510, 122, 58, 28}, "自动", kDark,
                   [this] { auto_calibration_gain(); },
                   calibration_selection_enabled && calibration_.band != "low");
        add_button({574, 122, 42, 28}, "-", kDark,
                   [this] { set_calibration_gain(-10); },
                   calibration_selection_enabled && calibration_.band != "low");
        add_button({622, 122, 42, 28}, "+", kDark,
                   [this] { set_calibration_gain(10); },
                   calibration_selection_enabled && calibration_.band != "low");
        draw_text_vcentered_clipped(calibration_.band == "low" ? "固定" : "启动时写入",
                                    672, kMuted, {672, 122, 96, 28}, font_small_);
        add_button({24, 154, 90, 28}, "开始", kGreen,
                   [this] { begin_selected_calibration(); },
                   controls_enabled() && calibration_.session_id.empty());
        const bool step_available = calibration_step_available(calibration_);
        add_button({120, 154, 90, 28}, "提交", step_available ? kCyan : kDark,
                   [this] { step_rx_calibration(); },
                   controls_enabled() && step_available);
        add_button({216, 154, 90, 28}, "保存",
                   calibration_.completed_points == calibration_required_count(calibration_.band) ? kGreen : kDark,
                   [this] { save_rx_calibration(); },
                   controls_enabled() && calibration_.completed_points == calibration_required_count(calibration_.band) &&
                       !calibration_.session_id.empty());
        add_button({312, 154, 90, 28}, "取消", kDark,
                   [this] { cancel_rx_calibration(); }, controls_enabled());
        std::ostringstream status;
        status << "CAL " << calibration_.state << "  " << calibration_.completed_points
               << "点  下一个 ";
        if (calibration_.next_input_dbm) {
            status << *calibration_.next_input_dbm << " dBm";
        } else {
            status << "--";
        }
        draw_text_clipped(status.str(), 416, 158, kMuted, {416, 154, 350, 28}, font_small_);

        if (calibration_.band == "low") {
            draw_calibration_table("low", kLowCalibrationInputDbm, 24);
        } else if (calibration_.band == "medium") {
            draw_calibration_table("medium", kMediumCalibrationInputDbm, 24);
        } else {
            draw_calibration_table("high", kHighCalibrationInputDbm, 24);
        }
    }

    void draw_calibration_leave_dialog()
    {
        draw_box({88, 148, 624, 188}, kPanel);
        draw_text("校准尚未保存", 112, 168, kAmber, font_bold_);
        draw_text_clipped("当前校准会话已有数据，离开前请选择保存或放弃。",
                          112, 204, kText, {112, 200, 560, 26}, font_small_);
        draw_text_clipped(calibration_.completed_points == calibration_required_count(calibration_.band)
                              ? "保存后会写入转发配置文件。"
                              : "未完成当前档全部校准点时只能放弃或继续校准。",
                          112, 230, kMuted, {112, 226, 560, 24}, font_small_);
        add_button({116, 278, 150, 38}, "继续校准", kDark,
                   [this] { calibration_leave_dialog_ = false; }, true);
        add_button({324, 278, 150, 38}, "放弃返回", kRed,
                   [this] { discard_calibration_and_exit(); }, controls_enabled());
        add_button({532, 278, 150, 38}, "保存返回",
                   calibration_.completed_points == calibration_required_count(calibration_.band) ? kGreen : kDark,
                   [this] { save_calibration_and_exit(); },
                   controls_enabled() && calibration_.completed_points == calibration_required_count(calibration_.band) &&
                       !calibration_.session_id.empty());
    }

    std::string call_mode_text(const std::string& mode) const
    {
        if (mode == "private") return "DMR 私呼";
        if (mode == "group") return "DMR 组呼";
        if (mode == "all_call") return "DMR 全呼";
        if (mode == "fm") return "FM 转 DMR 全呼";
        return mode;
    }

    void draw_navigation()
    {
        constexpr std::array<const char*, 4> labels{{"主页", "信道", "参数", "状态"}};
        const bool navigation_enabled = page_ != 4 || pending_.empty();
        for (int index = 0; index < 4; ++index) {
            add_button({index * 200, 440, 200, 40}, labels[static_cast<std::size_t>(index)],
                       page_ == index || (index == 3 && page_ == 4) ? kCyan : kDark, [this, index] {
                if (page_ == 4) {
                    request_calibration_exit(index);
                    return;
                }
                page_ = index;
                if (index == 1) request_channel_summaries();
                if (index == 2 && detail_profile_id_.empty()) detail_profile_id_ = state_.active_profile;
            }, navigation_enabled);
        }
    }

    void draw()
    {
        hits_.clear();
        SDL_SetRenderTarget(renderer_, canvas_);
        SDL_SetRenderDrawColor(renderer_, kBackground.r, kBackground.g, kBackground.b, kBackground.a);
        SDL_RenderClear(renderer_);
        draw_header();
        switch (page_) {
        case 0: draw_home(); break;
        case 1: draw_channel_page(); break;
        case 2: draw_parameter_page(); break;
        case 3: draw_status_page(); break;
        case 4: draw_calibration_page(); break;
        default: draw_home(); break;
        }
        draw_navigation();
        if (calibration_leave_dialog_) {
            calibration_dialog_hit_begin_ = hits_.size();
            draw_calibration_leave_dialog();
        } else {
            calibration_dialog_hit_begin_ = hits_.size();
        }
        if (direct_framebuffer_output_) {
#if defined(__linux__)
            if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                     framebuffer_pixels_.data(), kWidth * 4) != 0) {
                throw std::runtime_error("cannot read rendered GUI pixels");
            }
            for (int y = 0; y < kHeight; ++y) {
                std::memcpy(framebuffer_map_ + static_cast<std::size_t>(y) *
                                framebuffer_fix_.line_length,
                            framebuffer_pixels_.data() + static_cast<std::size_t>(y) *
                                kWidth * 4U,
                            static_cast<std::size_t>(kWidth) * 4U);
            }
#endif
            return;
        }
        SDL_SetRenderTarget(renderer_, nullptr);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        int physical_width = 0;
        int physical_height = 0;
        SDL_GetRendererOutputSize(renderer_, &physical_width, &physical_height);
        SDL_Rect target{0, 0, physical_width, physical_height};
        if (config_.rotation_degrees == 90 || config_.rotation_degrees == 270) {
            target = {(physical_width - physical_height) / 2, (physical_height - physical_width) / 2,
                      physical_height, physical_width};
        }
        SDL_RenderCopyEx(renderer_, canvas_, nullptr, &target, config_.rotation_degrees, nullptr, SDL_FLIP_NONE);
        SDL_RenderPresent(renderer_);
    }

    std::pair<int, int> logical_point(int physical_x, int physical_y) const
    {
        int width = 0;
        int height = 0;
        SDL_GetRendererOutputSize(renderer_, &width, &height);
        double x = static_cast<double>(physical_x);
        double y = static_cast<double>(physical_y);
        if (config_.rotation_degrees == 90) {
            return {static_cast<int>(y * kWidth / height), static_cast<int>((width - x) * kHeight / width)};
        }
        if (config_.rotation_degrees == 180) {
            return {static_cast<int>((width - x) * kWidth / width), static_cast<int>((height - y) * kHeight / height)};
        }
        if (config_.rotation_degrees == 270) {
            return {static_cast<int>((height - y) * kWidth / height), static_cast<int>(x * kHeight / width)};
        }
        return {static_cast<int>(x * kWidth / width), static_cast<int>(y * kHeight / height)};
    }

#if defined(__linux__)
    void open_touch_input()
    {
        for (int index = 0; index < 32; ++index) {
            const std::string path = "/dev/input/event" + std::to_string(index);
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            char name[256]{};
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
                (std::string(name).find("ft5x06") != std::string::npos ||
                 std::string(name).find("ft5506") != std::string::npos ||
                 std::string(name).find("edt-ft") != std::string::npos)) {
                touch_fd_ = fd;
                input_absinfo x_info{};
                input_absinfo y_info{};
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &x_info) == 0) {
                    touch_x_min_ = x_info.minimum;
                    touch_x_max_ = x_info.maximum;
                }
                if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &y_info) == 0) {
                    touch_y_min_ = y_info.minimum;
                    touch_y_max_ = y_info.maximum;
                }
                std::cerr << "dmr_b210_gui: direct touchscreen=" << path << '\n';
                return;
            }
            ::close(fd);
        }
        std::cerr << "dmr_b210_gui: direct touchscreen not available\n";
    }

    void handle_direct_touch()
    {
        if (touch_fd_ < 0) return;
        input_event event{};
        while (::read(touch_fd_, &event, sizeof(event)) == sizeof(event)) {
            if (event.type == EV_ABS && event.code == ABS_MT_POSITION_X) touch_x_ = event.value;
            if (event.type == EV_ABS && event.code == ABS_MT_POSITION_Y) touch_y_ = event.value;
            if (event.type == EV_KEY && event.code == BTN_TOUCH && event.value == 0) {
                const int x = std::clamp((touch_x_ - touch_x_min_) * kWidth /
                                             std::max(1, touch_x_max_ - touch_x_min_),
                                         0, kWidth - 1);
                const int y = std::clamp((touch_y_ - touch_y_min_) * kHeight /
                                             std::max(1, touch_y_max_ - touch_y_min_),
                                         0, kHeight - 1);
                dispatch_pointer_up(x, y);
            }
        }
    }
#endif

    void dispatch_pointer_up(int x, int y)
    {
        const auto now = std::chrono::steady_clock::now();
        if (last_pointer_up_.time_since_epoch().count() != 0 &&
            now - last_pointer_up_ < std::chrono::milliseconds(300) &&
            std::abs(x - last_pointer_x_) <= 8 &&
            std::abs(y - last_pointer_y_) <= 8) {
            return;
        }
        last_pointer_up_ = now;
        last_pointer_x_ = x;
        last_pointer_y_ = y;
        if (calibration_leave_dialog_) {
            for (std::size_t index = calibration_dialog_hit_begin_;
                 index < hits_.size(); ++index) {
                const Hit& hit = hits_[index];
                if (x >= hit.box.x && x < hit.box.x + hit.box.w &&
                    y >= hit.box.y && y < hit.box.y + hit.box.h) {
                    hit.callback();
                    return;
                }
            }
            return;
        }
        for (const Hit& hit : hits_) {
            if (x >= hit.box.x && x < hit.box.x + hit.box.w &&
                y >= hit.box.y && y < hit.box.y + hit.box.h) {
                hit.callback();
                return;
            }
        }
    }

    int event_loop()
    {
        bool running = true;
        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    if (page_ == 4 && calibration_has_unsaved_work()) {
                        request_calibration_exit(3);
                    } else {
                        running = false;
                    }
                } else if (event.type == SDL_MOUSEBUTTONUP || event.type == SDL_FINGERUP) {
                    const int raw_x = event.type == SDL_MOUSEBUTTONUP ? event.button.x :
                        static_cast<int>(event.tfinger.x * kWidth);
                    const int raw_y = event.type == SDL_MOUSEBUTTONUP ? event.button.y :
                        static_cast<int>(event.tfinger.y * kHeight);
                    const auto [x, y] = logical_point(raw_x, raw_y);
                    dispatch_pointer_up(x, y);
                }
            }
#if defined(__linux__)
            handle_direct_touch();
#endif
            handle_network();
            draw();
            SDL_Delay(20);
        }
        return 0;
    }

    struct Hit {
        SDL_Rect box{};
        std::function<void()> callback;
    };

    GuiConfig config_;
    UdpClient client_;
    bool stop_only_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* canvas_ = nullptr;
    bool direct_framebuffer_output_ = false;
#if defined(__linux__)
    int framebuffer_fd_ = -1;
    fb_fix_screeninfo framebuffer_fix_{};
    fb_var_screeninfo framebuffer_var_{};
    std::uint8_t* framebuffer_map_ = nullptr;
    std::vector<std::uint8_t> framebuffer_pixels_;
    int touch_fd_ = -1;
    int touch_x_ = 0;
    int touch_y_ = 0;
    int touch_x_min_ = 0;
    int touch_x_max_ = kWidth - 1;
    int touch_y_min_ = 0;
    int touch_y_max_ = kHeight - 1;
#endif
    TTF_Font* font_ = nullptr;
    TTF_Font* font_small_ = nullptr;
    TTF_Font* font_bold_ = nullptr;
    TTF_Font* font_frequency_ = nullptr;
    TTF_Font* font_frequency_digits_ = nullptr;
    RuntimeState state_;
    CalibrationUiState calibration_;
    std::map<std::string, std::string> pending_;
    std::map<std::string, std::chrono::steady_clock::time_point> pending_sent_at_;
    std::map<std::string, std::string> pending_operations_;
    std::map<std::string, Channel> channels_;
    std::map<std::string, std::string> channel_requests_;
    std::map<std::string, std::string> switch_requests_;
    std::string latest_switch_request_id_;
    std::optional<std::string> pending_switch_profile_;
    std::string pending_switch_action_;
    bool pending_switch_accepted_ = false;
    bool pending_switch_state_confirmed_ = false;
    std::chrono::steady_clock::time_point pending_switch_started_{};
    std::optional<bool> pending_forwarding_target_;
    std::string pending_forwarding_request_id_;
    std::string pending_forwarding_action_;
    bool pending_forwarding_accepted_ = false;
    std::chrono::steady_clock::time_point pending_forwarding_started_{};
    bool forwarding_enabled_known_ = false;
    bool rf_running_known_ = false;
    bool startup_forwarding_announced_ = false;
    std::map<std::string, std::chrono::steady_clock::time_point> channel_refresh_due_;
    std::vector<EventLine> events_;
    std::vector<Hit> hits_;
    std::vector<std::string> quick_profile_ids_;
    std::optional<std::string> calibration_query_request_;
    std::size_t calibration_dialog_hit_begin_ = 0;
    int page_ = 0;
    int channel_page_ = 0;
    std::string selected_profile_;
    std::string detail_profile_id_;
    Channel draft_channel_;
    std::string draft_profile_id_;
    bool draft_dirty_ = false;
    bool calibration_leave_dialog_ = false;
    bool calibration_exit_after_save_ = false;
    bool calibration_exit_after_discard_ = false;
    int calibration_exit_target_page_ = 3;
    bool qa_view_enabled_ = false;
    std::chrono::steady_clock::time_point last_pointer_up_{};
    int last_pointer_x_ = 0;
    int last_pointer_y_ = 0;
    std::chrono::steady_clock::time_point active_call_started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point gui_started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_calibration_query_{};
    std::chrono::steady_clock::time_point calibration_query_sent_at_{};
    std::chrono::steady_clock::time_point last_connection_attempt_{};
    bool ui_locked_ = false;
};

void usage(const char* program)
{
    std::cout << "Usage: " << program
              << " [--config PATH] [--stop-forwarding] [--self-test]"
                 " [--interaction-self-test] [--qa-view VIEW]\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path config_path = "/etc/dmr-rpt/gui.yaml";
    bool stop_only = false;
    bool self_test = false;
    bool interaction_self_test = false;
    std::string qa_view;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) config_path = argv[++index];
        else if (argument == "--stop-forwarding") stop_only = true;
        else if (argument == "--self-test") self_test = true;
        else if (argument == "--interaction-self-test") interaction_self_test = true;
        else if (argument == "--qa-view" && index + 1 < argc) qa_view = argv[++index];
        else if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (self_test) {
        if (GuiApp::self_test()) {
            std::cout << "GUI contract tests passed\n";
            return 0;
        }
        std::cerr << "GUI contract tests failed\n";
        return 1;
    }
    try {
        if (interaction_self_test) {
            GuiConfig interaction_config;
            try {
                if (std::filesystem::exists(config_path)) {
                    interaction_config = load_config(config_path);
                }
            } catch (const std::exception& error) {
                std::cerr << "GUI interaction self-test config fallback: " << error.what() << '\n';
            }
            if (GuiApp::interaction_self_test(std::move(interaction_config))) {
                std::cout << "GUI interaction tests passed\n";
                return 0;
            }
            std::cerr << "GUI interaction tests failed\n";
            return 1;
        }
        GuiApp app(load_config(config_path), stop_only);
        if (!qa_view.empty()) app.configure_qa_view(qa_view);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "dmr_b210_gui: " << error.what() << '\n';
        return 1;
    }
}
