// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/remote_voice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle_t = SOCKET;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle_t = int;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

namespace dmr_rpt {
namespace {

constexpr std::size_t kHeaderSize = 96U;
constexpr char kFeatureMagic[] = "DMR-RPT-AMBE-RECORDING-V1-000001";

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

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i32(std::vector<std::uint8_t>& output, std::int32_t value)
{
    append_u32(output, static_cast<std::uint32_t>(value));
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& input,
                       std::size_t offset)
{
    return static_cast<std::uint16_t>(
        input.at(offset) | (static_cast<std::uint16_t>(input.at(offset + 1U))
                             << 8U));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& input,
                       std::size_t offset)
{
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(input.at(offset + shift / 8U))
            << shift;
    }
    return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& input,
                       std::size_t offset)
{
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(input.at(offset + shift / 8U))
            << shift;
    }
    return value;
}

std::filesystem::path unique_path(const std::filesystem::path& directory,
                                  const std::string& filename)
{
    for (unsigned suffix = 0; suffix < 10000U; ++suffix) {
        const std::string suffix_text = suffix == 0U
            ? std::string{}
            : "_" + std::to_string(suffix);
        const std::filesystem::path candidate =
            directory / (std::filesystem::path(filename).stem().string() +
                         suffix_text + ".ambe");
        if (!std::filesystem::exists(candidate) &&
            !std::filesystem::exists(candidate.string() + ".part")) {
            return candidate;
        }
    }
    throw std::runtime_error("cannot allocate AMBE recording filename");
}

std::uint32_t crc32_update(std::uint32_t crc,
                           const std::uint8_t* data,
                           std::size_t size)
{
    while (size-- > 0U) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^
                (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

bool send_all(socket_handle_t socket,
              const char* data,
              std::size_t size)
{
    while (size > 0U) {
#if defined(_WIN32)
        const int sent = ::send(socket, data, static_cast<int>(size), 0);
#else
        const ssize_t sent = ::send(socket, data, size, MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool receive_line(socket_handle_t socket, std::string& line)
{
    line.clear();
    char ch = 0;
    while (line.size() < 256U) {
#if defined(_WIN32)
        const int received = ::recv(socket, &ch, 1, 0);
#else
        const ssize_t received = ::recv(socket, &ch, 1, 0);
#endif
        if (received <= 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    return false;
}

void set_socket_timeout(socket_handle_t socket, int timeout_ms)
{
#if defined(_WIN32)
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

socket_handle_t connect_to(const std::string& address, int port,
                           int timeout_ms)
{
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return kInvalidSocket;
    }
#endif
    socket_handle_t socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalidSocket) {
        return kInvalidSocket;
    }
    set_socket_timeout(socket, timeout_ms);
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

std::array<std::uint8_t, 9> pack_ambe_frame(const AmbeFrameDibits& frame)
{
    std::array<std::uint8_t, 9> packed{};
    for (std::size_t index = 0; index < frame.size(); ++index) {
        const std::size_t byte_index = index / 4U;
        const unsigned shift = static_cast<unsigned>(6U - (index % 4U) * 2U);
        packed[byte_index] |= static_cast<std::uint8_t>(
            (frame[index] & 0x03U) << shift);
    }
    return packed;
}

std::vector<std::uint8_t> serialize_ambe_header(
    const AmbeRecordingHeader& header)
{
    std::vector<std::uint8_t> output;
    output.reserve(kHeaderSize);
    std::string feature = header.feature.empty()
        ? std::string(kFeatureMagic)
        : header.feature;
    feature.resize(32U, '\0');
    output.insert(output.end(), feature.begin(), feature.begin() + 32);
    append_u16(output, header.version);
    append_u16(output, static_cast<std::uint16_t>(kHeaderSize));
    append_u32(output, header.flags);
    append_u64(output, header.started_unix_ms);
    append_u32(output, header.source_id);
    append_u32(output, header.destination_id);
    append_u32(output, header.repeater_id);
    append_i32(output, header.average_rssi_millidbfs);
    append_i32(output, header.latitude_e7);
    append_i32(output, header.longitude_e7);
    append_u32(output, header.duration_ms);
    append_u32(output, header.ambe_frame_count);
    append_u32(output, header.payload_size);
    append_u32(output, header.payload_crc32);
    append_u16(output, static_cast<std::uint16_t>(header.slot));
    output.push_back(static_cast<std::uint8_t>(header.color_code));
    output.push_back(static_cast<std::uint8_t>(header.mode));
    output.resize(kHeaderSize, 0U);
    return output;
}

AmbeRecordingHeader parse_ambe_header(
    const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < kHeaderSize) {
        throw std::runtime_error("AMBE header is shorter than 96 bytes");
    }
    const std::string feature(reinterpret_cast<const char*>(bytes.data()), 32);
    if (feature != std::string(kFeatureMagic, 32U)) {
        throw std::runtime_error("AMBE header feature string is invalid");
    }
    const std::uint16_t header_size = read_u16(bytes, 34U);
    if (header_size != kHeaderSize) {
        throw std::runtime_error("AMBE header size is unsupported");
    }
    if (read_u16(bytes, 32U) != 1U) {
        throw std::runtime_error("AMBE header version is unsupported");
    }
    for (std::size_t index = 91U; index < 96U; ++index) {
        if (bytes.at(index) != 0U) {
            throw std::runtime_error("AMBE header reserved bytes are non-zero");
        }
    }
    AmbeRecordingHeader header;
    header.feature = feature;
    header.version = read_u16(bytes, 32U);
    header.flags = read_u32(bytes, 36U);
    header.started_unix_ms = read_u64(bytes, 40U);
    header.source_id = read_u32(bytes, 48U);
    header.destination_id = read_u32(bytes, 52U);
    header.repeater_id = read_u32(bytes, 56U);
    header.average_rssi_millidbfs =
        static_cast<std::int32_t>(read_u32(bytes, 60U));
    header.latitude_e7 = static_cast<std::int32_t>(read_u32(bytes, 64U));
    header.longitude_e7 = static_cast<std::int32_t>(read_u32(bytes, 68U));
    header.duration_ms = read_u32(bytes, 72U);
    header.ambe_frame_count = read_u32(bytes, 76U);
    header.payload_size = read_u32(bytes, 80U);
    header.payload_crc32 = read_u32(bytes, 84U);
    header.slot = bytes.at(88U);
    header.color_code = bytes.at(89U);
    header.mode = static_cast<RecordingMode>(bytes.at(90U));
    return header;
}

AmbeRecordingWriter::~AmbeRecordingWriter()
{
    abort();
}

void AmbeRecordingWriter::start(const RecordingMetadata& metadata,
                                const RemoteVoiceConfig& config)
{
    abort();
    if (!config.enabled) {
        return;
    }
    std::filesystem::path directory = config.spool_directory;
    std::filesystem::create_directories(directory);
    final_path_ = unique_path(directory, format_ambe_recording_filename(metadata));
    partial_path_ = final_path_.string() + ".part";
    stream_.open(partial_path_, std::ios::in | std::ios::out |
                               std::ios::binary | std::ios::trunc);
    if (!stream_) {
        throw std::runtime_error("cannot create AMBE recording file");
    }

    header_.feature = config.feature;
    header_.version = 1;
    header_.started_unix_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            metadata.started_at.time_since_epoch()).count());
    header_.source_id = metadata.source_id;
    header_.destination_id = metadata.destination_id;
    header_.repeater_id = metadata.repeater_id;
    header_.average_rssi_millidbfs = static_cast<std::int32_t>(
        std::llround(metadata.average_rssi_dbfs * 1000.0));
    header_.latitude_e7 = metadata.latitude_e7;
    header_.longitude_e7 = metadata.longitude_e7;
    header_.slot = metadata.slot;
    header_.color_code = metadata.color_code;
    header_.mode = metadata.mode;
    const std::vector<std::uint8_t> bytes = serialize_ambe_header(header_);
    stream_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!stream_) {
        abort();
        throw std::runtime_error("cannot write AMBE recording header");
    }
    crc_state_ = 0xFFFFFFFFU;
    rssi_power_sum_ = 0.0;
    rssi_sample_count_ = 0;
    active_ = true;
}

void AmbeRecordingWriter::observe_rssi(double rssi_dbfs)
{
    if (!active_ || !std::isfinite(rssi_dbfs)) {
        return;
    }
    const double bounded_dbfs = std::clamp(rssi_dbfs, -200.0, 20.0);
    rssi_power_sum_ += std::pow(10.0, bounded_dbfs / 10.0);
    ++rssi_sample_count_;
}

void AmbeRecordingWriter::submit_burst(const DmrBurstDibits& burst)
{
    if (!active_) {
        return;
    }
    const AmbeBurstFrames frames = extract_ambe_frames(burst);
    for (const AmbeFrameDibits& frame : frames) {
        const auto packed = pack_ambe_frame(frame);
        stream_.write(reinterpret_cast<const char*>(packed.data()),
                      static_cast<std::streamsize>(packed.size()));
        if (!stream_) {
            throw std::runtime_error("cannot write AMBE recording payload");
        }
        crc_state_ = crc32_update(crc_state_, packed.data(), packed.size());
        ++header_.ambe_frame_count;
        header_.payload_size += static_cast<std::uint32_t>(packed.size());
    }
}

std::filesystem::path AmbeRecordingWriter::finish(std::int64_t duration_ms)
{
    if (!active_) {
        return {};
    }
    header_.duration_ms = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(duration_ms, 0, 0xFFFFFFFFLL));
    if (rssi_sample_count_ > 0U) {
        const double average_power = rssi_power_sum_ /
            static_cast<double>(rssi_sample_count_);
        header_.average_rssi_millidbfs = static_cast<std::int32_t>(
            std::llround(10.0 * std::log10(std::max(average_power, 1e-20)) *
                         1000.0));
    }
    header_.payload_crc32 = crc_state_ ^ 0xFFFFFFFFU;
    stream_.seekp(0, std::ios::beg);
    const std::vector<std::uint8_t> bytes = serialize_ambe_header(header_);
    stream_.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    stream_.flush();
    stream_.close();
    if (!stream_ && std::filesystem::exists(partial_path_)) {
        throw std::runtime_error("cannot finalize AMBE recording");
    }
    std::error_code error;
    std::filesystem::rename(partial_path_, final_path_, error);
    if (error) {
        throw std::runtime_error(
            "cannot rename AMBE recording: " + error.message());
    }
    active_ = false;
    return final_path_;
}

void AmbeRecordingWriter::abort() noexcept
{
    if (stream_.is_open()) {
        stream_.close();
    }
    if (!partial_path_.empty()) {
        std::error_code error;
        std::filesystem::remove(partial_path_, error);
    }
    active_ = false;
    rssi_power_sum_ = 0.0;
    rssi_sample_count_ = 0;
    final_path_.clear();
    partial_path_.clear();
}

bool AmbeRecordingWriter::active() const
{
    return active_;
}

RemoteVoiceUploader::RemoteVoiceUploader(RemoteVoiceConfig config)
    : config_(std::move(config))
{
}

void RemoteVoiceUploader::upload(const std::filesystem::path& file) const
{
    if (!config_.enabled) {
        return;
    }
    const std::uintmax_t size = std::filesystem::file_size(file);
    if (size > static_cast<std::uintmax_t>(
                   std::numeric_limits<std::uint32_t>::max()) * 64U) {
        throw std::runtime_error("AMBE recording is too large to upload");
    }
    const socket_handle_t socket =
        connect_to(config_.server_address, config_.server_port,
                   config_.connect_timeout_ms);
    if (socket == kInvalidSocket) {
        throw std::runtime_error("cannot connect to remote AMBE server");
    }
    const std::string filename = file.filename().string();
    const std::string command = "PUT " + filename + " " +
        std::to_string(size) + "\n";
    if (!send_all(socket, command.data(), command.size())) {
        close_socket(socket);
        throw std::runtime_error("cannot send remote AMBE PUT command");
    }
    set_socket_timeout(socket, config_.upload_timeout_ms);
    std::string response;
    if (!receive_line(socket, response) || response != "READY") {
        close_socket(socket);
        throw std::runtime_error("remote AMBE server did not accept upload");
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        close_socket(socket);
        throw std::runtime_error("cannot open AMBE recording for upload");
    }
    std::array<char, 65536> buffer{};
    while (input) {
        input.read(buffer.data(),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        if (read <= 0) {
            break;
        }
        if (!send_all(socket, buffer.data(), static_cast<std::size_t>(read))) {
            close_socket(socket);
            throw std::runtime_error("remote AMBE upload failed");
        }
    }
    if (!receive_line(socket, response) || response.rfind("OK", 0U) != 0U) {
        close_socket(socket);
        throw std::runtime_error("remote AMBE server rejected recording");
    }
    close_socket(socket);
}

} // namespace dmr_rpt
