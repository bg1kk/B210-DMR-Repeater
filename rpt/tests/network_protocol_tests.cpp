// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/network_protocol.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle_t = SOCKET;
using socket_length_t = int;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_handle_t = int;
using socket_length_t = socklen_t;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

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

void set_receive_timeout(socket_handle_t socket, int timeout_ms)
{
#if defined(_WIN32)
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

std::string receive_datagram(socket_handle_t socket)
{
    char buffer[4096]{};
    const int received = static_cast<int>(::recvfrom(
        socket, buffer, sizeof(buffer), 0, nullptr, nullptr));
    if (received <= 0) {
        return {};
    }
    return std::string(buffer, static_cast<std::size_t>(received));
}

void send_datagram(socket_handle_t socket, const sockaddr_in& endpoint,
                   const std::string& body)
{
    const int sent = static_cast<int>(::sendto(
        socket, body.data(), static_cast<int>(body.size()), 0,
        reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)));
    require(sent == static_cast<int>(body.size()), "UDP send failed");
}

sockaddr_in loopback_endpoint(int port)
{
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(static_cast<std::uint16_t>(port));
    require(::inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr) == 1,
            "cannot parse loopback address");
    return endpoint;
}

int local_port(socket_handle_t socket)
{
    sockaddr_in endpoint{};
    socket_length_t length = sizeof(endpoint);
    require(::getsockname(socket, reinterpret_cast<sockaddr*>(&endpoint),
                          &length) == 0,
            "cannot read bound UDP port");
    return static_cast<int>(ntohs(endpoint.sin_port));
}

void test_udp_status_query()
{
    socket_handle_t listener = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(listener != kInvalidSocket, "cannot create UDP listener");
    try {
        const sockaddr_in listener_endpoint = loopback_endpoint(0);
        require(::bind(listener,
                       reinterpret_cast<const sockaddr*>(&listener_endpoint),
                       sizeof(listener_endpoint)) == 0,
                "cannot bind UDP listener");
        set_receive_timeout(listener, 1200);
        const int listener_port = local_port(listener);

        const socket_handle_t port_reservation = ::socket(AF_INET, SOCK_DGRAM, 0);
        require(port_reservation != kInvalidSocket,
                "cannot reserve UDP control port");
        const sockaddr_in reserved_endpoint = loopback_endpoint(0);
        require(::bind(port_reservation,
                       reinterpret_cast<const sockaddr*>(&reserved_endpoint),
                       sizeof(reserved_endpoint)) == 0,
                "cannot bind UDP control port reservation");
        const int server_port = local_port(port_reservation);
        close_socket(port_reservation);

        dmr_rpt::UdpControlConfig udp_config;
        udp_config.bind_address = "127.0.0.1";
        udp_config.port = server_port;
        udp_config.control_token = "network-test-token";
        dmr_rpt::TcpStatusConfig tcp_config;
        tcp_config.enabled = false;
        dmr_rpt::NetworkControlCallbacks callbacks;
        bool persisted_switch_requested = false;
        std::string requested_gain_mode;
        callbacks.command = [&persisted_switch_requested, &requested_gain_mode](
                                const dmr_rpt::NetworkControlCommand& command) {
            dmr_rpt::NetworkControlResult result;
            if (command.operation == "switch_channel") {
                require(command.profile_id == "channel-02",
                        "switch_channel profile is incorrect");
                persisted_switch_requested =
                    command.persist_active_profile.value_or(false);
                result.accepted = true;
                result.code = "accepted";
                return result;
            }
            if (command.operation == "get_gain_mode") {
                result.accepted = true;
                result.code = "ok";
                result.state_json =
                    "{\"mode\":\"high\",\"high_gain_tenths_db\":250,"
                    "\"low_gain_tenths_db\":0}";
                return result;
            }
            if (command.operation == "get_version") {
                result.accepted = true;
                result.code = "ok";
                result.state_json =
                    "{\"repeater_version\":\"V1.0.7\","
                    "\"build_sequence\":109}";
                return result;
            }
            if (command.operation == "set_gain_mode") {
                require(command.gain_mode == "low",
                        "set_gain_mode value is incorrect");
                requested_gain_mode = command.gain_mode;
                result.accepted = true;
                result.code = "accepted";
                return result;
            }
            if (command.operation == "save_channel") {
                require(command.profile_id == "channel-02" &&
                            command.channel_patch.rx_frequency_hz.value_or(0) == 438512500,
                        "save_channel fields are incorrect");
                result.accepted = true;
                result.code = "accepted";
                return result;
            }
            require(false, "unexpected UDP control operation");
            return result;
        };
        callbacks.gain_control_json = [] {
            return std::string(
                "{\"mode\":\"high\",\"high_gain_tenths_db\":250,"
                "\"low_gain_tenths_db\":0}");
        };
        callbacks.status_json = [] {
            return std::string(
                "{\"v\":1,\"type\":\"status\",\"rf_running\":false,"
                "\"rf_fault\":true,\"last_error\":\"B210 unavailable\"}");
        };
        dmr_rpt::NetworkControlService service(
            udp_config, tcp_config, "9001", callbacks);
        service.start();
        try {
            dmr_rpt::ReceiveStatus dmr_call_status{
                0, "dmr", true, -60.0, 30.0};
            dmr_call_status.hardware_agc_enabled = true;
            dmr_call_status.analog_gain_db = 25.0;
            dmr_call_status.software_agc_gain_db = 6.5;
            dmr_call_status.agc_input_dbfs = -61.2;
            dmr_call_status.rssi_gain_compensation_db = -5.0;
            dmr_call_status.active_call = {
                100103, 100102, "group"};
            dmr_call_status.active_call_state_known = true;
            service.observe_receive_status(dmr_call_status);
            service.observe_receive_status({
                1, "fm", false, -90.0, 0.0});

            const sockaddr_in server_endpoint = loopback_endpoint(server_port);
            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"start\",\"op\":\"start_status_query\","
                "\"listen_port\":" + std::to_string(listener_port) +
                ",\"token\":\"network-test-token\"}");
            const std::string start_response = receive_datagram(listener);
            require(start_response.find("\"code\":\"ok\"") != std::string::npos &&
                        start_response.find(
                            "destination_port\":" +
                            std::to_string(listener_port)) !=
                            std::string::npos,
                    "start_status_query response is invalid");

            unsigned status_frames = 0;
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(1200);
            while (std::chrono::steady_clock::now() < deadline) {
                service.observe_receive_status(dmr_call_status);
                service.observe_receive_status({
                    1, "fm", false, -90.0, 0.0});
                const std::string frame = receive_datagram(listener);
                if (frame.empty()) {
                    break;
                }
                if (frame.find("\"type\":\"status_stream\"") ==
                    std::string::npos) {
                    continue;
                }
                ++status_frames;
                require(frame.find("\"interval_ms\":200") != std::string::npos &&
                            frame.find("\"working_mode\":\"dmr_receive\"") !=
                                std::string::npos &&
                            frame.find("\"rssi_dbfs\":-60.00") !=
                                std::string::npos &&
                            frame.find("\"snr_db\":30.00") !=
                                std::string::npos &&
                            frame.find("\"hardware_agc_enabled\":true") !=
                                std::string::npos &&
                            frame.find("\"analog_gain_db\":25.00") !=
                                std::string::npos &&
                            frame.find("\"software_agc_gain_db\":6.50") !=
                                std::string::npos &&
                            frame.find("\"agc_input_dbfs\":-61.20") !=
                                std::string::npos &&
                            frame.find("\"rssi_gain_compensation_db\":-5.00") !=
                                std::string::npos &&
                            frame.find("\"receiver_mode\":\"dmr\"") !=
                                std::string::npos &&
                            frame.find("\"gain_control\":{\"mode\":\"high\"") !=
                                std::string::npos &&
                            frame.find("\"rf_fault\":true") != std::string::npos &&
                            frame.find("B210 unavailable") != std::string::npos &&
                            frame.find("\"active_call\":{\"rx_channel\":0,"
                                       "\"source_id\":100103,"
                                       "\"destination_id\":100102,"
                                       "\"mode\":\"group\"}") !=
                                std::string::npos,
                        "UDP status payload is incomplete");
            }
            require(status_frames >= 5U,
                    "UDP status query did not publish at 5 Hz");

            service.publish_relay_event({
                "started", dmr_rpt::RelaySource::Dmr, 100103, 100102,
                1, 1, 0, -60.0, "network-test-call"});
            service.observe_receive_status({
                0, "dmr", true, -60.0, 30.0});
            const std::string relay_frame = receive_datagram(listener);
            require(relay_frame.find("\"working_mode\":\"dmr_relay\"") !=
                        std::string::npos,
                    "UDP status did not enter DMR relay mode");
            service.publish_relay_event({
                "ended", dmr_rpt::RelaySource::Dmr, 100103, 100102,
                1, 1, 600, -60.0, "network-test-call"});
            service.observe_receive_status({
                0, "dmr", false, -90.0, 0.0});
            dmr_rpt::ReceiveStatus fm_call_status{
                1, "fm", true, -55.0, 18.0};
            fm_call_status.active_call = {9999, 0xFFFFFFU, "fm"};
            fm_call_status.active_call_state_known = true;
            service.observe_receive_status(fm_call_status);
            std::string fm_call_frame;
            const auto fm_call_deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(1200);
            while (std::chrono::steady_clock::now() < fm_call_deadline) {
                fm_call_frame = receive_datagram(listener);
                if (fm_call_frame.find("\"type\":\"status_stream\"") !=
                        std::string::npos &&
                    fm_call_frame.find("\"active_call\":{\"rx_channel\":1,"
                                       "\"source_id\":9999,"
                                       "\"destination_id\":16777215,"
                                       "\"mode\":\"fm\"}") !=
                        std::string::npos) {
                    break;
                }
            }
            require(fm_call_frame.find("\"mode\":\"fm\"") !=
                        std::string::npos,
                    "UDP status did not report the FM call identity");

            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"stop\",\"op\":\"stop_status_query\","
                "\"token\":\"network-test-token\"}");
            std::string stop_response;
            do {
                stop_response = receive_datagram(listener);
            } while (!stop_response.empty() &&
                     stop_response.find("\"request_id\":\"stop\"") ==
                         std::string::npos);
            require(stop_response.find("\"code\":\"ok\"") != std::string::npos,
                    "stop_status_query response is invalid");
            set_receive_timeout(listener, 350);
            require(receive_datagram(listener).empty(),
                    "UDP status query continued after stop");

            set_receive_timeout(listener, 1200);
            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"get-gain\",\"op\":\"get_gain_mode\","
                "\"token\":\"network-test-token\"}");
            const std::string get_gain_response = receive_datagram(listener);
            require(get_gain_response.find("\"code\":\"ok\"") !=
                        std::string::npos &&
                        get_gain_response.find("\"mode\":\"high\"") !=
                            std::string::npos,
                    "get_gain_mode response is incomplete");

            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"get-version\",\"op\":\"get_version\","
                "\"token\":\"network-test-token\"}");
            const std::string version_response = receive_datagram(listener);
            require(version_response.find("\"repeater_version\":\"V1.0.7\"") !=
                        std::string::npos &&
                    version_response.find("\"build_sequence\":109") !=
                        std::string::npos,
                    "get_version response is incomplete");

            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"set-gain\",\"op\":\"set_gain_mode\","
                "\"gain_mode\":\"low\","
                "\"token\":\"network-test-token\"}");
            const std::string set_gain_response = receive_datagram(listener);
            require(set_gain_response.find("\"code\":\"accepted\"") !=
                        std::string::npos &&
                        requested_gain_mode == "low",
                    "set_gain_mode request is not parsed");

            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"save-channel\",\"op\":\"save_channel\","
                "\"profile_id\":\"channel-02\",\"rx_frequency_hz\":438512500,"
                "\"token\":\"network-test-token\"}");
            const std::string save_channel_response = receive_datagram(listener);
            require(save_channel_response.find("\"code\":\"accepted\"") !=
                        std::string::npos,
                    "save_channel request is parsed");

            send_datagram(
                listener, server_endpoint,
                "{\"v\":1,\"id\":\"switch\",\"op\":\"switch_channel\","
                "\"profile_id\":\"channel-02\","
                "\"persist_active_profile\":true,"
                "\"token\":\"network-test-token\"}");
            const std::string switch_response = receive_datagram(listener);
            require(switch_response.find("\"code\":\"accepted\"") !=
                        std::string::npos &&
                        persisted_switch_requested,
                    "switch_channel persistence option is not parsed");
        } catch (...) {
            service.stop();
            throw;
        }
        service.stop();
    } catch (...) {
        close_socket(listener);
        throw;
    }
    close_socket(listener);
}

} // namespace

int main()
{
    try {
#if defined(_WIN32)
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
#endif
        test_udp_status_query();
#if defined(_WIN32)
        WSACleanup();
#endif
        std::cout << "network protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "network_protocol_tests: " << error.what() << '\n';
#if defined(_WIN32)
        WSACleanup();
#endif
        return 1;
    }
}
