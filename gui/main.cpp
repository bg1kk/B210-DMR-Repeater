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
#include <unistd.h>

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
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
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
    std::string device_name = "DMR B210 转发器";
    std::string udp_address = "127.0.0.1";
    int udp_port = 42000;
    int listen_port = 43000;
    std::filesystem::path control_token_file;
    std::string control_token;
    std::string calibration_password = "14254328";
    int rotation_degrees = 0;
    int kmsdrm_device_index = -1;
    std::string font_path =
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    std::array<double, 2> s9_reference_dbfs{-87.0, -87.0};
    std::array<double, 2> s9_reference_dbm{-73.0, -73.0};
    std::vector<std::string> profile_ids;
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

GuiConfig load_config(const std::filesystem::path& path)
{
    const YAML::Node root = YAML::LoadFile(path.string());
    const YAML::Node gui = root["gui"] ? root["gui"] : root;
    GuiConfig config;
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
    if (gui["font_path"]) config.font_path = gui["font_path"].as<std::string>();
    if (gui["s9_reference_dbfs"] && gui["s9_reference_dbfs"].IsSequence()) {
        for (std::size_t index = 0; index < 2U && index < gui["s9_reference_dbfs"].size(); ++index) {
            config.s9_reference_dbfs[index] = gui["s9_reference_dbfs"][index].as<double>();
        }
    }
    if (gui["s9_reference_dbm"] && gui["s9_reference_dbm"].IsSequence()) {
        for (std::size_t index = 0; index < 2U && index < gui["s9_reference_dbm"].size(); ++index) {
            config.s9_reference_dbm[index] = gui["s9_reference_dbm"][index].as<double>();
        }
    }
    if (gui["profile_ids"] && gui["profile_ids"].IsSequence()) {
        for (const YAML::Node& id : gui["profile_ids"]) {
            config.profile_ids.push_back(id.as<std::string>());
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
    return config;
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

const std::array<int, 14> kCalibrationInputDbm = {
    0, -10, -20, -30, -40, -50, -60, -70, -80, -90, -100, -110, -120, -121
};

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
    bool stale = true;
    std::string active_profile;
    std::string repeater_version = "读取中";
    int repeater_build_sequence = 0;
    std::string working_mode = "idle";
    std::string gain_mode = "custom";
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
    std::string band = "strong";
    std::string session_id;
    std::string state = "idle";
    int next_input_dbm = 0;
    int completed_points = 0;
    int gain_tenths_db = 0;
    std::array<std::optional<int>, 4> column_gain{};
    std::array<std::map<int, std::pair<double, double>>, 4> column_points{};
    int selected_column = 0;
    std::optional<int> selected_input_dbm;
    int scroll_row = 0;
    bool authorized = false;
    std::string password_input;
    std::string password_error;
};

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
        return level.label == "S9" && level.lit_segments == 9 &&
            s_meter(-67.0, -87.0).label == "S9 +20 dB" &&
            s_meter(-140.0, -87.0).lit_segments == 0 &&
            elapsed_clock(started) == "01:01:01";
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
        if (config_.kmsdrm_device_index >= 0 &&
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
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer_) throw std::runtime_error("cannot create kiosk renderer");
        canvas_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, kWidth, kHeight);
        if (!canvas_) throw std::runtime_error("cannot create kiosk canvas");
        font_ = TTF_OpenFont(config_.font_path.c_str(), 16);
        if (!font_) throw std::runtime_error("cannot open configured Chinese font");
        font_small_ = TTF_OpenFont(config_.font_path.c_str(), 13);
        font_bold_ = TTF_OpenFont(config_.font_path.c_str(), 19);
        if (!font_small_ || !font_bold_) throw std::runtime_error("cannot open kiosk font sizes");
    }

    void shutdown_sdl()
    {
        if (font_bold_) TTF_CloseFont(font_bold_);
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

    void track(const std::string& id, const std::string& action)
    {
        pending_[id] = action;
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
        refresh_due_channels();
        const auto now = std::chrono::steady_clock::now();
        const bool stale = state_.last_update.time_since_epoch().count() == 0 ||
            now - state_.last_update > std::chrono::seconds(1);
        if (stale != state_.stale) {
            state_.stale = stale;
            if (stale) {
                state_.active_call = {};
                add_event("状态订阅已中断", kRed);
            }
        }
        state_.online = !state_.stale;
    }

    void update_runtime(const std::string& object)
    {
        if (const auto profile = json_string(object, "active_channel_profile_id")) state_.active_profile = *profile;
        if (const auto count = json_number<int>(object, "configured_channel_profile_count")) state_.configured_profiles = *count;
        if (const auto bytes = json_number<std::uint64_t>(object, "recording_storage_bytes")) {
            state_.recording_storage_bytes = *bytes;
        }
        if (const auto limit = json_number<std::uint64_t>(object, "recording_storage_limit_bytes")) {
            state_.recording_storage_limit_bytes = *limit;
        }
        if (const auto forwarding = json_bool(object, "forwarding_enabled")) state_.forwarding_enabled = *forwarding;
        if (const auto running = json_bool(object, "rf_running")) state_.rf_running = *running;
        if (const auto error = json_string(object, "last_error")) state_.last_error = *error;
        if (const auto version = json_string(object, "repeater_version")) {
            state_.repeater_version = *version;
        }
        if (const auto build = json_number<int>(object, "build_sequence")) {
            state_.repeater_build_sequence = *build;
        }
        const std::string gain = json_object(object, "gain_control");
        if (!gain.empty()) {
            if (const auto mode = json_string(gain, "mode")) state_.gain_mode = *mode;
        }
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
            if (make_active) state_.active_channel = channel;
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
                (band != "strong" && band != "weak")) continue;
            const int column = rx_channel * 2 + (band == "weak" ? 1 : 0);
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
        const int session_column = calibration_.rx_channel * 2 +
            (calibration_.band == "weak" ? 1 : 0);
        if (session_column >= 0 && session_column < 4 && !session_points.empty()) {
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
        const auto channel_request = channel_requests_.find(request_id);
        const std::string requested_profile = channel_request == channel_requests_.end() ? "" :
            channel_request->second;
        if (channel_request != channel_requests_.end()) channel_requests_.erase(channel_request);
        const auto pending = pending_.find(request_id);
        const std::string action = pending == pending_.end() ? "UDP 命令" : pending->second;
        if (pending != pending_.end()) pending_.erase(pending);
        const bool ok = json_bool(frame, "ok").value_or(false);
        const std::string code = json_string(frame, "code").value_or("错误");
        const std::string message = json_string(frame, "message").value_or("");
        if (ok) add_event(action + "：" + (message.empty() ? code : message), kGreen);
        else add_event(action + "失败：" + (message.empty() ? code : message), kRed);
        const std::string state = json_object(frame, "state");
        if (!state.empty()) {
            update_calibration_state(state);
            if (state.find("\"state\"") != std::string::npos &&
                state.find("\"dmr_rx\"") == std::string::npos) {
                calibration_.state = json_string(state, "state").value_or("idle");
                calibration_.session_id = json_string(state, "session_id").value_or("");
                calibration_.band = json_string(state, "band").value_or(calibration_.band);
                calibration_.rx_channel = json_number<int>(state, "rx_channel").value_or(calibration_.rx_channel);
                calibration_.next_input_dbm = json_number<int>(state, "next_input_dbm").value_or(0);
                calibration_.completed_points = json_number<int>(state, "completed_points").value_or(0);
                calibration_.gain_tenths_db = json_number<int>(state, "rx_gain_tenths_db").value_or(0);
                if (state.find("\"next_input_dbm\":null") != std::string::npos) {
                    calibration_.selected_input_dbm.reset();
                } else {
                    calibration_.selected_input_dbm = calibration_.next_input_dbm;
                }
            }
            if (request_id.find("channel") != std::string::npos ||
                state.find("\"dmr_rx\"") != std::string::npos) {
                update_channel(state, requested_profile.empty() ||
                    requested_profile == state_.active_profile);
            }
            update_runtime(state);
        }
        if (action == "CAL begin" || action == "CAL step" ||
            action == "CAL commit" || action == "CAL cancel" ||
            action == "CAL gain" || action == "CAL auto") {
            try {
                track(client_.send("get_rx_calibration"), "CAL query");
            } catch (...) {
            }
        }
    }

    bool controls_enabled() const
    {
        return state_.online && pending_.empty();
    }

    void send_control(const std::string& operation, const std::string& fields,
                      const std::string& description)
    {
        if (!controls_enabled()) return;
        try {
            track(client_.send(operation, fields), description);
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

    void draw_box(SDL_Rect box, SDL_Color fill = kPanel)
    {
        SDL_SetRenderDrawColor(renderer_, fill.r, fill.g, fill.b, fill.a);
        SDL_RenderFillRect(renderer_, &box);
        SDL_SetRenderDrawColor(renderer_, kLine.r, kLine.g, kLine.b, kLine.a);
        SDL_RenderDrawRect(renderer_, &box);
    }

    void add_button(SDL_Rect box, const std::string& label, SDL_Color color,
                    std::function<void()> callback, bool enabled = true,
                    bool available_when_locked = false)
    {
        draw_box(box, enabled ? color : kDark);
        int width = 0;
        int height = 0;
        TTF_SizeUTF8(font_small_, label.c_str(), &width, &height);
        const int x = box.x + std::max(4, (box.w - width) / 2);
        const int y = box.y + std::max(2, (box.h - height) / 2);
        draw_text_clipped(label, x, y, enabled ? kText : kMuted,
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
        const std::string channel_title = state_.active_profile.empty() ? "读取中" :
            state_.active_profile + "  " + frequency_text(state_.active_channel.tx_frequency_hz);
        draw_text_clipped(channel_title, 28, 78, kText, {28, 74, 420, 30}, font_bold_);
        draw_text_clipped("RX " + frequency_text(state_.active_channel.rx_frequency_hz) +
                          "   |   TX " + frequency_text(state_.active_channel.tx_frequency_hz),
                          28, 110, kMuted, {28, 106, 430, 24}, font_small_);
        draw_box({28, 134, 430, 1}, kLine);
        for (std::size_t index = 0; index < std::min<std::size_t>(3, config_.profile_ids.size()); ++index) {
            const int x = 28 + static_cast<int>(index) * 134;
            add_button({x, 144, 133, 28}, config_.profile_ids[index],
                config_.profile_ids[index] == state_.active_profile ? kCyan : kDark,
                [this, index] { request_switch(config_.profile_ids[index]); }, controls_enabled());
        }
        draw_text_vcentered_clipped("转发控制", 28, kMuted, {28, 175, 229, 19},
                                    font_small_);
        draw_text_vcentered_clipped("增益：" + state_.gain_mode, 368, kMuted,
                                    {368, 175, 88, 19}, font_small_);
        const SDL_Rect forwarding_state_box{28, 198, 229, 35};
        draw_box(forwarding_state_box, state_.forwarding_enabled ? kGreen : kDark);
        draw_text_vcentered_clipped(
            state_.forwarding_enabled ? "● 转发运行中" : "○ 转发待机", 44,
            state_.forwarding_enabled ? kBackground : kText,
            {40, forwarding_state_box.y, 205, forwarding_state_box.h}, font_);
        add_button({267, 198, 126, 35}, state_.forwarding_enabled ? "停止转发" : "启动转发",
                   state_.forwarding_enabled ? kDark : kGreen, [this] {
            send_control(state_.forwarding_enabled ? "stop_forwarding" : "start_forwarding", {},
                         state_.forwarding_enabled ? "停止转发" : "启动转发");
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
        draw_text_clipped(state_.gain_mode, 608, 302, kText, {608, 300, 58, 20}, font_small_);
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
                                        ? config_.s9_reference_dbm[static_cast<std::size_t>(receiver)]
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
                                        ? config_.s9_reference_dbm[static_cast<std::size_t>(source)]
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
            add_button({x, y, 356, 54}, "", active ? kCyan : kPanel,
                       [this, profile_id] { open_channel_details(profile_id); }, controls_enabled());
            draw_text(channel_label(profile_id), x + 14, y + 10, active ? kBackground : kText, font_bold_);
            const auto found = channels_.find(profile_id);
            if (found == channels_.end()) {
                draw_text("RX / TX 读取中", x + 84, y + 18, kMuted, font_small_);
            } else {
                draw_text_clipped("RX " + frequency_text(found->second.rx_frequency_hz) + "  |  TX " +
                                  frequency_text(found->second.tx_frequency_hz),
                                  x + 84, y + 18, active ? kBackground : kMuted,
                                  {x + 84, y + 14, 254, 28}, font_small_);
            }
            if (active) draw_text("当前", x + 14, y + 34, kBackground, font_small_);
        }
    }

    void request_switch(const std::string& id, bool persist_active_profile = false)
    {
        selected_profile_ = id;
        send_control("switch_channel", "\"profile_id\":\"" + json_escape(id) + "\",\"persist_active_profile\":" +
                     (persist_active_profile ? "true" : "false"),
                     persist_active_profile ? "设置开机信道" : "切换信道");
    }

    std::string channel_label(const std::string& profile_id) const
    {
        const auto found = std::find(config_.profile_ids.begin(), config_.profile_ids.end(), profile_id);
        if (found == config_.profile_ids.end()) return profile_id;
        return "CH" + std::to_string(std::distance(config_.profile_ids.begin(), found) + 1);
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
            track(request_id, description);
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
            track(request_id, description);
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
                   [this, profile_id] { request_switch(profile_id); }, controls_enabled());
        add_button({568, 306, 188, 34}, "设为开机信道", kDark,
                   [this, profile_id] { request_switch(profile_id, true); }, controls_enabled());
        add_button({202, 366, 180, 34}, "高档", state_.gain_mode == "high" ? kCyan : kDark,
                   [this] { send_control("set_gain_mode", "\"gain_mode\":\"high\"", "切换高增益"); },
                   controls_enabled() && editing_active);
        add_button({418, 366, 180, 34}, "低档", state_.gain_mode == "low" ? kCyan : kDark,
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

    void begin_rx_calibration()
    {
        send_control("rx_calibration_begin",
                     "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
                         ",\"calibration_band\":\"" + calibration_.band + "\"",
                     "CAL begin");
    }

    void step_rx_calibration()
    {
        if (calibration_.session_id.empty()) return;
        send_control("rx_calibration_step",
                     "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
                         ",\"calibration_band\":\"" + calibration_.band +
                         "\",\"session_id\":\"" + calibration_.session_id +
                         "\",\"input_dbm\":" + std::to_string(calibration_.next_input_dbm) +
                         ",\"rx_gain_tenths_db\":" + std::to_string(calibration_.gain_tenths_db),
                     "CAL step");
    }

    void commit_rx_calibration()
    {
        if (calibration_.session_id.empty()) return;
        send_control("rx_calibration_commit",
                     "\"session_id\":\"" + calibration_.session_id + "\"",
                     "CAL commit");
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
        calibration_.authorized = false;
        calibration_.password_input.clear();
        calibration_.password_error.clear();
    }

    void select_calibration_column(int column)
    {
        if (column < 0 || column >= 4) return;
        calibration_.selected_column = column;
        calibration_.rx_channel = column / 2;
        calibration_.band = column % 2 == 0 ? "strong" : "weak";
        calibration_.selected_input_dbm.reset();
    }

    void set_calibration_gain(int delta_tenths_db)
    {
        const int column = calibration_.selected_column;
        const int current = calibration_.column_gain[static_cast<std::size_t>(column)].value_or(
            calibration_.gain_tenths_db);
        const int next = std::clamp(current + delta_tenths_db, 0, 1000);
        send_control("set_rx_gain",
                     "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
                         ",\"calibration_band\":\"" + calibration_.band +
                         "\",\"rx_gain_tenths_db\":" + std::to_string(next),
                     "CAL gain");
        calibration_.gain_tenths_db = next;
    }

    void auto_calibration_gain()
    {
        const Receiver& receiver = state_.receivers[static_cast<std::size_t>(calibration_.rx_channel)];
        if (!receiver.rssi_valid) {
            add_event("CAL auto: RSSI unavailable", kRed);
            return;
        }
        const int current = calibration_.column_gain[static_cast<std::size_t>(calibration_.selected_column)]
            .value_or(calibration_.gain_tenths_db);
        const int next = std::clamp(current + static_cast<int>(std::lround((-20.0 - receiver.rssi_dbfs) * 10.0)),
                                    0, 1000);
        send_control("set_rx_gain",
                     "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
                         ",\"calibration_band\":\"" + calibration_.band +
                         "\",\"rx_gain_tenths_db\":" + std::to_string(next),
                     "CAL auto");
        calibration_.gain_tenths_db = next;
    }

    void submit_calibration_password()
    {
        if (calibration_.password_input == config_.calibration_password) {
            calibration_.authorized = true;
            calibration_.password_error.clear();
            try {
                track(client_.send("get_rx_calibration"), "CAL query");
            } catch (...) {
            }
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
        std::string fields = "\"rx_channel\":" + std::to_string(calibration_.rx_channel) +
            ",\"calibration_band\":\"" + calibration_.band + "\"";
        if (calibration_.selected_input_dbm) {
            fields += ",\"start_input_dbm\":" +
                std::to_string(*calibration_.selected_input_dbm);
        }
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
            track(request_id, "保存 " + channel_label(profile_id));
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
                  24, 88, state_.stale ? kRed : kGreen, font_);
        for (int index = 0; index < 2; ++index) {
            const Receiver& receiver = state_.receivers[static_cast<std::size_t>(index)];
            const int y = 116 + index * 102;
            draw_box({24, y, 468, 92});
            draw_text("RX" + std::to_string(index + 1) + " " + receiver_mode_label(receiver.mode),
                      38, y + 11, receiver.receiving ? kGreen : kMuted, font_);
            if (receiver.rssi_valid) {
                std::ostringstream text;
                const bool calibrated = receiver.calibration_state == "calibrated" &&
                    receiver.rssi_dbm_valid;
                text << std::fixed << std::setprecision(1) << "RSSI "
                     << (calibrated ? receiver.rssi_dbm : receiver.rssi_dbfs)
                     << (calibrated ? " dBm" : " dBFS U")
                     << "  SNR " << (receiver.snr_valid ? receiver.snr_db : 0.0)
                     << " dB";
                draw_text_clipped(text.str(), 38, y + 42, kText, {38, y + 38, 270, 24}, font_small_);
            } else {
                draw_text("RSSI/SNR 未知", 38, y + 42, kMuted, font_small_);
            }
            draw_meter(334, y + 10, index);
        }
        draw_box({510, 116, 266, 202});
        draw_text("呼叫与故障", 524, 129, kMuted, font_small_);
        draw_text(state_.active_call.valid ? call_mode_text(state_.active_call.mode) : "无有效呼叫",
                  524, 156, state_.active_call.valid ? kGreen : kMuted, font_);
        if (state_.active_call.valid) {
            draw_text_clipped("源 " + std::to_string(state_.active_call.source_id) + "  目标 " +
                              std::to_string(state_.active_call.destination_id),
                              524, 184, kText, {524, 182, 236, 22}, font_small_);
        }
        draw_text_clipped("工作方式：" + state_.working_mode, 524, 216, kCyan,
                          {524, 214, 236, 22}, font_small_);
        draw_text_clipped(state_.last_error.empty() ? "最近错误：无" : "最近错误：" + state_.last_error,
                          524, 248, state_.last_error.empty() ? kGreen : kRed,
                          {524, 246, 236, 44}, font_small_);
        add_button({510, 334, 266, 38}, "RSSI校准", kDark,
                   [this] { open_calibration_page(); }, controls_enabled());
    }

    void draw_calibration_password_page()
    {
        draw_box({12, 46, 776, 382});
        draw_text("RSSI校准", 28, 62, kText, font_bold_);
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
        add_button({28, 382, 128, 32}, "返回状态", kDark,
                   [this] { page_ = 3; });
    }

    std::string calibration_column_label(int column) const
    {
        const int rx = column / 2 + 1;
        return "RX" + std::to_string(rx) + (column % 2 == 0 ? " 高档" : " 低档");
    }

    void draw_calibration_page()
    {
        if (!calibration_.authorized) {
            draw_calibration_password_page();
            return;
        }
        draw_box({12, 46, 776, 382});
        draw_text("RSSI校准", 24, 58, kText, font_bold_);
        draw_text("当前输入", 24, 86, kMuted, font_small_);
        const Receiver& receiver = state_.receivers[static_cast<std::size_t>(calibration_.rx_channel)];
        std::ostringstream live;
        live << "RX" << (calibration_.rx_channel + 1) << "  " << std::fixed
             << std::setprecision(1)
             << (receiver.rssi_valid ? receiver.rssi_dbfs : 0.0) << " dBFS  SNR "
             << (receiver.snr_valid ? receiver.snr_db : 0.0) << " dB";
        draw_text_clipped(receiver.rssi_valid ? live.str() : "RX --", 104, 86, kCyan,
                          {104, 82, 300, 24}, font_small_);
        add_button({24, 112, 76, 30}, "RX1", calibration_.rx_channel == 0 ? kCyan : kDark,
                   [this] { select_calibration_column(calibration_.selected_column % 2); }, controls_enabled());
        add_button({108, 112, 76, 30}, "RX2", calibration_.rx_channel == 1 ? kCyan : kDark,
                   [this] { select_calibration_column(2 + calibration_.selected_column % 2); }, controls_enabled());
        add_button({196, 112, 76, 30}, "高档", calibration_.band == "strong" ? kCyan : kDark,
                   [this] { select_calibration_column(calibration_.rx_channel * 2); }, controls_enabled());
        add_button({280, 112, 76, 30}, "低档", calibration_.band == "weak" ? kCyan : kDark,
                   [this] { select_calibration_column(calibration_.rx_channel * 2 + 1); }, controls_enabled());
        const int column = calibration_.selected_column;
        const int gain = calibration_.column_gain[static_cast<std::size_t>(column)].value_or(
            calibration_.gain_tenths_db);
        draw_text("RX GAIN", 386, 86, kMuted, font_small_);
        draw_text(gain_text(gain), 464, 86, kCyan, font_bold_);
        add_button({386, 112, 68, 30}, "自动", kDark,
                   [this] { auto_calibration_gain(); }, controls_enabled());
        add_button({462, 112, 46, 30}, "-", kDark,
                   [this] { set_calibration_gain(-10); }, controls_enabled());
        add_button({514, 112, 46, 30}, "+", kDark,
                   [this] { set_calibration_gain(10); }, controls_enabled());
        draw_text_clipped("每列增益 " + gain_text(gain), 572, 116, kMuted,
                          {572, 112, 198, 30}, font_small_);
        add_button({24, 150, 90, 30}, "开始", kGreen,
                   [this] { begin_selected_calibration(); }, controls_enabled());
        add_button({120, 150, 90, 30}, "测量", calibration_.state == "active" ? kAmber : kDark,
                   [this] { step_rx_calibration(); }, controls_enabled());
        add_button({216, 150, 90, 30}, "提交", calibration_.state == "active" ? kGreen : kDark,
                   [this] { commit_rx_calibration(); }, controls_enabled());
        add_button({312, 150, 90, 30}, "取消", kDark,
                   [this] { cancel_rx_calibration(); }, controls_enabled());
        std::ostringstream status;
        status << "CAL " << calibration_.state << "  " << calibration_.completed_points
               << "点  下一个 " << calibration_.next_input_dbm << " dBm";
        draw_text_clipped(status.str(), 416, 154, kMuted, {416, 150, 350, 30}, font_small_);

        draw_box({24, 188, 728, 224});
        draw_text_vcentered_clipped("dBm", 34, kMuted, {34, 194, 45, 40}, font_small_);
        for (int index = 0; index < 4; ++index) {
            const int x = 88 + index * 162;
            draw_text_vcentered_clipped(calibration_column_label(index), x, kText,
                                        {x, 192, 150, 23}, font_small_);
            draw_text_vcentered_clipped(
                "G " + gain_text(calibration_.column_gain[static_cast<std::size_t>(index)].value_or(0)),
                x, kMuted, {x, 215, 150, 23}, font_small_);
        }
        constexpr int visible_rows = 8;
        const int max_scroll = static_cast<int>(kCalibrationInputDbm.size()) - visible_rows;
        calibration_.scroll_row = std::clamp(calibration_.scroll_row, 0, std::max(0, max_scroll));
        for (int row = 0; row < visible_rows; ++row) {
            const int input_index = calibration_.scroll_row + row;
            if (input_index >= static_cast<int>(kCalibrationInputDbm.size())) break;
            const int input = kCalibrationInputDbm[static_cast<std::size_t>(input_index)];
            const int y = 241 + row * 21;
            const bool selected = calibration_.selected_input_dbm &&
                *calibration_.selected_input_dbm == input;
            add_button({28, y, 720, 20}, "", selected ? kCyan : kPanel,
                       [this, input] { calibration_.selected_input_dbm = input; }, controls_enabled());
            draw_text(std::to_string(input), 34, y + 2, selected ? kBackground : kText, font_small_);
            for (int index = 0; index < 4; ++index) {
                const auto found = calibration_.column_points[static_cast<std::size_t>(index)].find(input);
                const int x = 88 + index * 162;
                if (found == calibration_.column_points[static_cast<std::size_t>(index)].end()) {
                    draw_text("--", x, y + 2, kMuted, font_small_);
                } else {
                    std::ostringstream value;
                    value << std::fixed << std::setprecision(1) << found->second.first;
                    draw_text(value.str(), x, y + 2, selected ? kBackground : kCyan, font_small_);
                }
            }
        }
        calibration_scroll_track_ = {756, 236, 20, 165};
        draw_box(*calibration_scroll_track_, kDark);
        const int thumb_height = 28;
        const int thumb_y = calibration_scroll_track_->y +
            (max_scroll == 0 ? 0 : calibration_.scroll_row *
             (calibration_scroll_track_->h - thumb_height) / max_scroll);
        draw_box({calibration_scroll_track_->x, thumb_y,
                  calibration_scroll_track_->w, thumb_height}, kCyan);
        add_button({24, 416, 128, 12}, "返回状态", kDark,
                   [this] { page_ = 3; }, controls_enabled());
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
        constexpr std::array<const char*, 5> labels{{"主页", "信道", "参数", "状态", "校准"}};
        for (int index = 0; index < 5; ++index) {
            add_button({index * 160, 440, 160, 40}, labels[static_cast<std::size_t>(index)],
                       page_ == index || (index == 3 && page_ == 4) ? kCyan : kDark, [this, index] {
                page_ = index;
                if (index == 1) request_channel_summaries();
                if (index == 2 && detail_profile_id_.empty()) detail_profile_id_ = state_.active_profile;
                if (index == 4) open_calibration_page();
            });
        }
    }

    void draw()
    {
        hits_.clear();
        calibration_scroll_track_.reset();
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

    int event_loop()
    {
        bool running = true;
        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    running = false;
                } else if (event.type == SDL_MOUSEBUTTONUP || event.type == SDL_FINGERUP) {
                    const int raw_x = event.type == SDL_MOUSEBUTTONUP ? event.button.x :
                        static_cast<int>(event.tfinger.x * kWidth);
                    const int raw_y = event.type == SDL_MOUSEBUTTONUP ? event.button.y :
                        static_cast<int>(event.tfinger.y * kHeight);
                    const auto [x, y] = logical_point(raw_x, raw_y);
                    if (page_ == 4 && calibration_.authorized && calibration_scroll_track_ &&
                        x >= calibration_scroll_track_->x &&
                        x < calibration_scroll_track_->x + calibration_scroll_track_->w &&
                        y >= calibration_scroll_track_->y &&
                        y < calibration_scroll_track_->y + calibration_scroll_track_->h) {
                        constexpr int visible_rows = 8;
                        const int max_scroll = static_cast<int>(kCalibrationInputDbm.size()) - visible_rows;
                        const int travel = std::max(1, calibration_scroll_track_->h - 28);
                        calibration_.scroll_row = std::clamp(
                            (y - calibration_scroll_track_->y - 14) * max_scroll / travel,
                            0, std::max(0, max_scroll));
                        continue;
                    }
                    for (const Hit& hit : hits_) {
                        if (x >= hit.box.x && x < hit.box.x + hit.box.w &&
                            y >= hit.box.y && y < hit.box.y + hit.box.h) {
                            hit.callback();
                            break;
                        }
                    }
                }
            }
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
    TTF_Font* font_ = nullptr;
    TTF_Font* font_small_ = nullptr;
    TTF_Font* font_bold_ = nullptr;
    RuntimeState state_;
    CalibrationUiState calibration_;
    std::map<std::string, std::string> pending_;
    std::map<std::string, Channel> channels_;
    std::map<std::string, std::string> channel_requests_;
    std::map<std::string, std::chrono::steady_clock::time_point> channel_refresh_due_;
    std::vector<EventLine> events_;
    std::vector<Hit> hits_;
    std::optional<SDL_Rect> calibration_scroll_track_;
    int page_ = 0;
    int channel_page_ = 0;
    std::string selected_profile_;
    std::string detail_profile_id_;
    Channel draft_channel_;
    std::string draft_profile_id_;
    bool draft_dirty_ = false;
    std::chrono::steady_clock::time_point active_call_started_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point gui_started_ = std::chrono::steady_clock::now();
    bool ui_locked_ = false;
};

void usage(const char* program)
{
    std::cout << "Usage: " << program << " [--config PATH] [--stop-forwarding] [--self-test]\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path config_path = "/etc/dmr-rpt/gui.yaml";
    bool stop_only = false;
    bool self_test = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) config_path = argv[++index];
        else if (argument == "--stop-forwarding") stop_only = true;
        else if (argument == "--self-test") self_test = true;
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
        GuiApp app(load_config(config_path), stop_only);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << "dmr_b210_gui: " << error.what() << '\n';
        return 1;
    }
}
