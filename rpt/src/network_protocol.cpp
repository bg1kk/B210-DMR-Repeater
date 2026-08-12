// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/network_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_length_t = int;
using socket_handle_t = SOCKET;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_length_t = socklen_t;
using socket_handle_t = int;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

namespace dmr_rpt {
namespace {

void close_socket(socket_handle_t socket)
{
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdown_socket(socket_handle_t socket)
{
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

bool send_all(socket_handle_t socket, const std::string& body)
{
    std::size_t offset = 0;
    while (offset < body.size()) {
#if defined(_WIN32)
        const int sent = ::send(socket, body.data() + offset,
                                 static_cast<int>(body.size() - offset), 0);
#else
        const ssize_t sent = ::send(socket, body.data() + offset,
                                     body.size() - offset, MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string json_escape(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << static_cast<unsigned>(ch)
                    << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string json_string(const std::string& value)
{
    return "\"" + json_escape(value) + "\"";
}

std::string optional_double_json(const std::optional<double>& value)
{
    if (!value) {
        return "null";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << *value;
    return out.str();
}

std::int64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string relay_event_json(const NetworkRelayEvent& event)
{
    std::ostringstream out;
    out << "{\"v\":1,\"type\":\"relay\",\"event\":"
        << json_string(event.event)
        << ",\"source\":" << json_string(to_string(event.source))
        << ",\"source_id\":" << event.source_id
        << ",\"destination_id\":" << event.destination_id
        << ",\"slot\":" << event.slot
        << ",\"color_code\":" << event.color_code
        << ",\"relay_duration_ms\":" << event.relay_duration_ms
        << ",\"average_rssi_dbfs\":"
        << optional_double_json(event.average_rssi_dbfs)
        << ",\"correlation_id\":" << json_string(event.correlation_id)
        << "}\n";
    return out.str();
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::optional<std::string> json_string_field(const std::string& json,
                                             const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string result;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaped) {
            switch (ch) {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back(ch); break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return result;
        } else {
            result.push_back(ch);
        }
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> json_integer_field(const std::string& json,
                                    const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    std::size_t consumed = 0;
    try {
        const long long value = std::stoll(json.substr(pos), &consumed, 10);
        if (consumed == 0U ||
            value < static_cast<long long>(std::numeric_limits<T>::lowest()) ||
            value > static_cast<long long>(std::numeric_limits<T>::max())) {
            return std::nullopt;
        }
        return static_cast<T>(value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> json_bool_field(const std::string& json,
                                    const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    const std::size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t pos = json.find(':', key_pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t')) {
        ++pos;
    }
    if (json.compare(pos, 4U, "true") == 0) {
        return true;
    }
    if (json.compare(pos, 5U, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<NetworkControlCommand> parse_command(const std::string& body)
{
    NetworkControlCommand command;
    command.request_id = json_string_field(body, "id").value_or("");
    command.operation = json_string_field(body, "op").value_or("");
    command.profile_id = json_string_field(body, "profile_id").value_or("");
    command.persist_active_profile =
        json_bool_field(body, "persist_active_profile");
    command.gain_mode = json_string_field(body, "gain_mode").value_or("");
    command.listen_port = json_integer_field<int>(body, "listen_port");
    command.calibration_rx_channel =
        json_integer_field<int>(body, "rx_channel");
    command.calibration_band =
        json_string_field(body, "calibration_band").value_or("");
    command.calibration_session_id =
        json_string_field(body, "session_id").value_or("");
    command.calibration_input_dbm =
        json_integer_field<int>(body, "input_dbm");
    command.calibration_start_input_dbm =
        json_integer_field<std::int32_t>(body, "start_input_dbm");
    command.calibration_rx_gain_tenths_db =
        json_integer_field<std::int32_t>(body, "rx_gain_tenths_db");
    if (command.operation.empty()) {
        return std::nullopt;
    }
    command.channel_patch.rx_frequency_hz =
        json_integer_field<std::int64_t>(body, "rx_frequency_hz");
    command.channel_patch.tx_frequency_hz =
        json_integer_field<std::int64_t>(body, "tx_frequency_hz");
    command.channel_patch.rx_gain_tenths_db =
        json_integer_field<std::int32_t>(body, "rx_gain_tenths_db");
    command.channel_patch.tx_gain_tenths_db =
        json_integer_field<std::int32_t>(body, "tx_gain_tenths_db");
    command.channel_patch.fm_enabled = json_bool_field(body, "fm_enabled");
    command.channel_patch.ctcss_tone_tenths_hz =
        json_integer_field<std::int32_t>(body, "ctcss_tone_tenths_hz");
    return command;
}

std::string response_json(const NetworkControlCommand& command,
                          const NetworkControlResult& result)
{
    std::ostringstream out;
    out << "{\"v\":1,\"type\":\"response\",\"ok\":"
        << (result.accepted ? "true" : "false")
        << ",\"request_id\":"
        << json_string(command.request_id)
        << ",\"code\":" << json_string(result.code)
        << ",\"message\":" << json_string(result.message);
    if (!result.state_json.empty()) {
        out << ",\"state\":" << result.state_json;
    }
    out << "}\n";
    return out.str();
}

void initialize_sockets()
{
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    });
#endif
}

socket_handle_t bind_socket(const std::string& address,
                            int port,
                            int type)
{
    initialize_sockets();
    socket_handle_t socket = ::socket(AF_INET, type, 0);
    if (socket == kInvalidSocket) {
        throw std::runtime_error("cannot create network socket");
    }
    int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1) {
        close_socket(socket);
        throw std::runtime_error("network bind address must be an IPv4 literal");
    }
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint),
               sizeof(endpoint)) != 0) {
        close_socket(socket);
        throw std::runtime_error("cannot bind network socket");
    }
    return socket;
}

bool wait_readable(socket_handle_t socket, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return ::select(static_cast<int>(socket) + 1, &set, nullptr, nullptr,
                    &timeout) > 0;
}

socket_handle_t connect_socket(const std::string& address, int port)
{
    initialize_sockets();
    socket_handle_t socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalidSocket) {
        return kInvalidSocket;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1 ||
        ::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint),
                  sizeof(endpoint)) != 0) {
        close_socket(socket);
        return kInvalidSocket;
    }
    return socket;
}

} // namespace

const char* to_string(RelaySource source)
{
    return source == RelaySource::Fm ? "fm" : "dmr";
}

struct TcpStatusPublisher::Impl {
    Impl(TcpStatusConfig value, std::string id, NetworkControlCallbacks cb)
        : config(std::move(value)), device_id(std::move(id)),
          callbacks(std::move(cb))
    {
    }

    ~Impl()
    {
        stop();
    }

    void start()
    {
        if (!config.enabled) {
            return;
        }
        if (config.interval_ms != 1000) {
            throw std::runtime_error("tcp_status.interval_ms must be 1000");
        }
        listen_socket = bind_socket(config.bind_address, config.port,
                                    SOCK_STREAM);
        if (::listen(listen_socket, config.maximum_clients) != 0) {
            close_socket(listen_socket);
            listen_socket = kInvalidSocket;
            throw std::runtime_error("cannot listen on TCP status socket");
        }
        stopping = false;
        worker = std::thread([this] { run(); });
    }

    void stop() noexcept
    {
        stopping = true;
        condition.notify_all();
        shutdown_socket(listen_socket);
        if (worker.joinable()) {
            worker.join();
        }
        close_socket(listen_socket);
        listen_socket = kInvalidSocket;
        for (const socket_handle_t socket : clients) {
            shutdown_socket(socket);
            close_socket(socket);
        }
        clients.clear();
    }

    bool is_running() const
    {
        return worker.joinable();
    }

    void observe_signal(int channel, double power)
    {
        std::lock_guard<std::mutex> lock(mutex);
        SignalWindow& window = signal_windows[channel];
        window.sum_power += std::max(power, 1e-20);
        ++window.count;
    }

    void publish_relay_event(const NetworkRelayEvent& event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outbound.push_back(relay_event_json(event));
        condition.notify_one();
    }

    std::string signal_json(int channel, std::optional<double> average)
    {
        std::ostringstream out;
        out << "{\"v\":1,\"type\":\"signal\",\"device_id\":"
            << json_string(device_id)
            << ",\"rx_channel\":" << channel
            << ",\"window_ms\":1000,\"average_rssi_dbfs\":"
            << optional_double_json(average) << "}\n";
        return out.str();
    }

    void run()
    {
        auto next_signal = std::chrono::steady_clock::now() +
            std::chrono::seconds(1);
        while (!stopping) {
            if (listen_socket != kInvalidSocket &&
                wait_readable(listen_socket, 100)) {
                sockaddr_in peer{};
                socket_length_t length = sizeof(peer);
                const socket_handle_t client = ::accept(
                    listen_socket, reinterpret_cast<sockaddr*>(&peer), &length);
                if (client != kInvalidSocket &&
                    clients.size() < static_cast<std::size_t>(
                        std::max(1, config.maximum_clients))) {
                    clients.push_back(client);
                    std::ostringstream hello;
                    hello << "{\"v\":1,\"type\":\"hello\",\"stream\":"
                          << json_string(config.protocol)
                          << ",\"device_id\":" << json_string(device_id)
                          << "}\n";
                    if (!send_all(client, hello.str())) {
                        shutdown_socket(client);
                        close_socket(client);
                        clients.pop_back();
                    }
                } else if (client != kInvalidSocket) {
                    shutdown_socket(client);
                    close_socket(client);
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_signal) {
                std::vector<std::string> frames;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto& [channel, window] : signal_windows) {
                        std::optional<double> average;
                        if (window.count > 0U) {
                            average = 10.0 * std::log10(window.sum_power /
                                static_cast<double>(window.count));
                        }
                        frames.push_back(signal_json(channel, average));
                        window = {};
                    }
                    while (!outbound.empty()) {
                        frames.push_back(std::move(outbound.front()));
                        outbound.pop_front();
                    }
                }
                if (callbacks.status_json) {
                    frames.push_back(callbacks.status_json() + "\n");
                }
                for (const std::string& frame : frames) {
                    std::vector<socket_handle_t> keep;
                    keep.reserve(clients.size());
                    for (const socket_handle_t client : clients) {
                        if (send_all(client, frame)) {
                            keep.push_back(client);
                        } else {
                            shutdown_socket(client);
                            close_socket(client);
                        }
                    }
                    clients.swap(keep);
                }
                do {
                    next_signal += std::chrono::seconds(1);
                } while (next_signal <= now);
            }
        }
    }

    struct SignalWindow {
        double sum_power = 0.0;
        std::uint64_t count = 0;
    };

    TcpStatusConfig config;
    std::string device_id;
    NetworkControlCallbacks callbacks;
    socket_handle_t listen_socket = kInvalidSocket;
    std::thread worker;
    std::atomic_bool stopping{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<int, SignalWindow> signal_windows;
    std::deque<std::string> outbound;
    std::vector<socket_handle_t> clients;
};

TcpStatusPublisher::TcpStatusPublisher(TcpStatusConfig config,
                                       std::string device_id,
                                       NetworkControlCallbacks callbacks)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(device_id), std::move(callbacks)))
{
}

TcpStatusPublisher::~TcpStatusPublisher() = default;
void TcpStatusPublisher::start() { impl_->start(); }
void TcpStatusPublisher::stop() noexcept { impl_->stop(); }
bool TcpStatusPublisher::running() const { return impl_->is_running(); }
void TcpStatusPublisher::observe_signal(int channel, double power)
{
    impl_->observe_signal(channel, power);
}
void TcpStatusPublisher::publish_relay_event(const NetworkRelayEvent& event)
{
    impl_->publish_relay_event(event);
}

struct UdpControlServer::Impl {
    struct StatusSubscription {
        sockaddr_in endpoint {};
    };

    struct TimedReceiveStatus {
        ReceiveStatus status;
        std::int64_t observed_at_ms = 0;
    };

    Impl(UdpControlConfig value, NetworkControlCallbacks cb)
        : config(std::move(value)), callbacks(std::move(cb))
    {
    }

    ~Impl()
    {
        stop();
    }

    void start()
    {
        if (!config.enabled) {
            return;
        }
        listen_socket = bind_socket(config.bind_address, config.port,
                                    SOCK_DGRAM);
        stopping = false;
        worker = std::thread([this] { run(); });
    }

    void stop() noexcept
    {
        stopping = true;
        shutdown_socket(listen_socket);
        if (worker.joinable()) {
            worker.join();
        }
        close_socket(listen_socket);
        listen_socket = kInvalidSocket;
    }

    bool is_running() const
    {
        return worker.joinable();
    }

    void observe_receive_status(const ReceiveStatus& status)
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        TimedReceiveStatus next{status, monotonic_ms()};
        const auto previous = receive_statuses.find(status.rx_channel);
        if (!next.status.active_call_state_known &&
            previous != receive_statuses.end()) {
            next.status.active_call = previous->second.status.active_call;
            next.status.active_call_state_known =
                previous->second.status.active_call_state_known;
        }
        if (!next.status.receiving) {
            next.status.active_call.reset();
            next.status.active_call_state_known = true;
        }
        receive_statuses[status.rx_channel] = std::move(next);
    }

    void observe_relay_event(const NetworkRelayEvent& event)
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (event.event == "started") {
            active_relay_mode = std::string(to_string(event.source)) + "_relay";
            active_relay_correlation_id = event.correlation_id;
        } else if (event.event == "ended" &&
                   (active_relay_correlation_id.empty() ||
                    active_relay_correlation_id == event.correlation_id)) {
            active_relay_mode.clear();
            active_relay_correlation_id.clear();
        }
    }

    bool authorized(const std::string& body,
                    const sockaddr_in& peer) const
    {
        char address[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
        const bool loopback = std::string(address) == "127.0.0.1";
        if (config.control_token.empty()) {
            return loopback;
        }
        return json_string_field(body, "token").value_or("") ==
            config.control_token;
    }

    NetworkControlResult start_status_query(
        const NetworkControlCommand& command, const sockaddr_in& peer)
    {
        NetworkControlResult result;
        if (!command.listen_port || *command.listen_port <= 0 ||
            *command.listen_port > 65535) {
            result.code = "bad_request";
            result.message =
                "start_status_query requires listen_port from 1 to 65535";
            return result;
        }
        StatusSubscription next;
        next.endpoint = peer;
        next.endpoint.sin_port =
            htons(static_cast<std::uint16_t>(*command.listen_port));
        subscription = next;
        next_status_at = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(200);
        result.accepted = true;
        result.code = "ok";
        result.message = "UDP status query started";
        result.state_json =
            "{\"stream\":\"dmr-rpt-udp-status/1\",\"interval_ms\":200,"
            "\"destination_port\":" + std::to_string(*command.listen_port) +
            "}";
        return result;
    }

    NetworkControlResult stop_status_query()
    {
        subscription.reset();
        NetworkControlResult result;
        result.accepted = true;
        result.code = "ok";
        result.message = "UDP status query stopped";
        result.state_json =
            "{\"stream\":\"dmr-rpt-udp-status/1\",\"active\":false}";
        return result;
    }

    std::string status_stream_json()
    {
        std::vector<TimedReceiveStatus> statuses;
        std::string relay_mode;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            statuses.reserve(receive_statuses.size());
            for (const auto& [channel, status] : receive_statuses) {
                (void)channel;
                statuses.push_back(status);
            }
            relay_mode = active_relay_mode;
        }
        std::sort(statuses.begin(), statuses.end(),
                  [](const TimedReceiveStatus& left,
                     const TimedReceiveStatus& right) {
                      return left.status.rx_channel < right.status.rx_channel;
                  });
        const std::int64_t now_ms = monotonic_ms();
        std::string working_mode = relay_mode;
        bool dmr_receiving = false;
        bool fm_receiving = false;
        std::optional<NetworkReceiveCall> active_call;
        int active_call_rx_channel = -1;
        for (const TimedReceiveStatus& status : statuses) {
            const bool fresh = now_ms >= status.observed_at_ms &&
                now_ms - status.observed_at_ms <= 1000;
            if (!fresh || !status.status.receiving) {
                continue;
            }
            dmr_receiving = dmr_receiving ||
                status.status.receiver_mode == "dmr";
            fm_receiving = fm_receiving ||
                status.status.receiver_mode == "fm";
            const bool prefer_dmr = active_call &&
                active_call->mode == "fm" &&
                status.status.receiver_mode == "dmr";
            if (status.status.active_call &&
                (!active_call || prefer_dmr)) {
                active_call = status.status.active_call;
                active_call_rx_channel = status.status.rx_channel;
            }
        }
        if (working_mode.empty()) {
            working_mode = dmr_receiving ? "dmr_receive" :
                fm_receiving ? "fm_receive" : "idle";
        }

        std::ostringstream out;
        out << "{\"v\":1,\"type\":\"status_stream\",\"stream\":"
            << json_string("dmr-rpt-udp-status/1")
            << ",\"sequence\":" << ++status_sequence
            << ",\"interval_ms\":200,\"working_mode\":"
            << json_string(working_mode)
            << ",\"receivers\":[";
        bool first = true;
        for (const TimedReceiveStatus& status : statuses) {
            const bool fresh = now_ms >= status.observed_at_ms &&
                now_ms - status.observed_at_ms <= 1000;
            const bool receiving = fresh && status.status.receiving;
            const std::optional<double> rssi = fresh
                ? status.status.rssi_dbfs : std::nullopt;
            const std::optional<double> snr = fresh
                ? status.status.snr_db : std::nullopt;
            const std::optional<double> rssi_dbm = fresh
                ? status.status.rssi_dbm : std::nullopt;
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"rx_channel\":" << status.status.rx_channel
                << ",\"receiving\":" << (receiving ? "true" : "false")
                << ",\"rssi_dbfs\":" << optional_double_json(rssi)
                << ",\"rssi_dbm\":" << optional_double_json(rssi_dbm)
                << ",\"snr_db\":" << optional_double_json(snr)
                << ",\"receiver_mode\":"
                << json_string(status.status.receiver_mode)
                << ",\"calibration_state\":"
                << json_string(fresh ? status.status.calibration_state
                                      : "uncalibrated") << '}';
        }
        out << ']';
        out << ",\"active_call\":";
        if (active_call) {
            out << "{\"rx_channel\":" << active_call_rx_channel
                << ",\"source_id\":" << active_call->source_id
                << ",\"destination_id\":" << active_call->destination_id
                << ",\"mode\":" << json_string(active_call->mode)
                << '}';
        } else {
            out << "null";
        }
        if (callbacks.gain_control_json) {
            const std::string gain_control = callbacks.gain_control_json();
            if (!gain_control.empty()) {
                out << ",\"gain_control\":" << gain_control;
            }
        }
        if (callbacks.status_json) {
            const std::string runtime = callbacks.status_json();
            if (!runtime.empty()) {
                out << ",\"runtime\":" << runtime;
            }
        }
        out << "}\n";
        return out.str();
    }

    void run()
    {
        const std::size_t maximum_size = static_cast<std::size_t>(
            std::max(1, config.maximum_datagram_bytes));
        // Windows discards an oversized UDP datagram with WSAEMSGSIZE when
        // recvfrom() is given a smaller buffer, preventing a too_large reply.
        std::vector<char> buffer(65536U);
        next_status_at = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(200);
        while (!stopping) {
            int wait_ms = 100;
            if (subscription) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_status_at) {
                    wait_ms = 0;
                } else {
                    wait_ms = static_cast<int>(std::min(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            next_status_at - now).count(),
                        static_cast<std::int64_t>(100)));
                }
            }
            if (listen_socket != kInvalidSocket &&
                wait_readable(listen_socket, wait_ms)) {
                sockaddr_in peer{};
                socket_length_t peer_length = sizeof(peer);
#if defined(_WIN32)
                const int received = ::recvfrom(
                    listen_socket, buffer.data(), static_cast<int>(buffer.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), &peer_length);
#else
                const ssize_t received = ::recvfrom(
                    listen_socket, buffer.data(), buffer.size(), 0,
                    reinterpret_cast<sockaddr*>(&peer), &peer_length);
#endif
                if (received > 0) {
                    if (static_cast<std::size_t>(received) > maximum_size) {
                        const char response[] =
                            "{\"v\":1,\"type\":\"response\",\"code\":\"too_large\"}\n";
                        ::sendto(listen_socket, response,
                                 static_cast<int>(sizeof(response) - 1U), 0,
                                 reinterpret_cast<const sockaddr*>(&peer),
                                 peer_length);
                    } else {
                        const std::string body(
                            buffer.data(), static_cast<std::size_t>(received));
                        NetworkControlResult result;
                        const auto command = parse_command(body);
                        if (!command) {
                            result.code = "bad_request";
                            result.message = "invalid command JSON";
                        } else if (!authorized(body, peer)) {
                            result.code = "forbidden";
                            result.message = "control authorization failed";
                        } else if (command->operation == "start_status_query") {
                            result = start_status_query(*command, peer);
                        } else if (command->operation == "stop_status_query") {
                            result = stop_status_query();
                        } else if (!callbacks.command) {
                            result.code = "not_implemented";
                            result.message =
                                "no command dispatcher is installed";
                        } else {
                            result = callbacks.command(*command);
                        }
                        const std::string response = command
                            ? response_json(*command, result)
                            : "{\"v\":1,\"type\":\"response\","
                              "\"code\":\"bad_request\"}\n";
                        ::sendto(listen_socket, response.data(),
                                 static_cast<int>(response.size()), 0,
                                 reinterpret_cast<const sockaddr*>(&peer),
                                 peer_length);
                    }
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (subscription && now >= next_status_at) {
                const std::string status = status_stream_json();
                ::sendto(listen_socket, status.data(),
                         static_cast<int>(status.size()), 0,
                         reinterpret_cast<const sockaddr*>(
                             &subscription->endpoint),
                         static_cast<socket_length_t>(
                             sizeof(subscription->endpoint)));
                do {
                    next_status_at += std::chrono::milliseconds(200);
                } while (next_status_at <= now);
            }
        }
    }

    UdpControlConfig config;
    NetworkControlCallbacks callbacks;
    socket_handle_t listen_socket = kInvalidSocket;
    std::thread worker;
    std::atomic_bool stopping{false};
    std::optional<StatusSubscription> subscription;
    std::chrono::steady_clock::time_point next_status_at {};
    std::mutex status_mutex;
    std::unordered_map<int, TimedReceiveStatus> receive_statuses;
    std::string active_relay_mode;
    std::string active_relay_correlation_id;
    std::uint64_t status_sequence = 0;
};

UdpControlServer::UdpControlServer(UdpControlConfig config,
                                   NetworkControlCallbacks callbacks)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(callbacks)))
{
}

UdpControlServer::~UdpControlServer() = default;
void UdpControlServer::start() { impl_->start(); }
void UdpControlServer::stop() noexcept { impl_->stop(); }
bool UdpControlServer::running() const { return impl_->is_running(); }
void UdpControlServer::observe_receive_status(const ReceiveStatus& status)
{
    impl_->observe_receive_status(status);
}
void UdpControlServer::observe_relay_event(const NetworkRelayEvent& event)
{
    impl_->observe_relay_event(event);
}

NetworkControlService::NetworkControlService(
    UdpControlConfig udp_config,
    TcpStatusConfig tcp_config,
    std::string device_id,
    NetworkControlCallbacks callbacks)
    : tcp_(std::make_shared<TcpStatusPublisher>(
          std::move(tcp_config), std::move(device_id), callbacks))
    , udp_(std::make_unique<UdpControlServer>(
          std::move(udp_config), std::move(callbacks)))
{
}

NetworkControlService::~NetworkControlService()
{
    stop();
}

void NetworkControlService::start()
{
    tcp_->start();
    try {
        udp_->start();
    } catch (...) {
        tcp_->stop();
        throw;
    }
}

void NetworkControlService::stop() noexcept
{
    if (udp_) {
        udp_->stop();
    }
    if (tcp_) {
        tcp_->stop();
    }
}

bool NetworkControlService::running() const
{
    return tcp_ && tcp_->running();
}

void NetworkControlService::observe_signal(int channel, double power)
{
    tcp_->observe_signal(channel, power);
}

void NetworkControlService::observe_receive_status(
    const ReceiveStatus& status)
{
    udp_->observe_receive_status(status);
}

void NetworkControlService::publish_relay_event(
    const NetworkRelayEvent& event)
{
    tcp_->publish_relay_event(event);
    udp_->observe_relay_event(event);
}

} // namespace dmr_rpt
