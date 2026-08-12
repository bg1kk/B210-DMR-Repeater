// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dmr_rpt {

enum class RecordingMode {
    DmrRelay,
    DmrDirect,
    FmRelay,
};

struct RecordingMetadata {
    RecordingMode mode = RecordingMode::DmrRelay;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    int color_code = 1;
    int slot = 1;
    std::string correlation_id;
    std::uint32_t repeater_id = 0;
    double average_rssi_dbfs = 0.0;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    std::chrono::system_clock::time_point started_at =
        std::chrono::system_clock::now();
};

using DmrBurstDibits = std::array<std::uint8_t, 132>;
using AmbeFrameDibits = std::array<std::uint8_t, 36>;
using AmbeBurstFrames = std::array<AmbeFrameDibits, 3>;

struct AmbeRecordingHeader {
    std::string feature = "DMR-RPT-AMBE-RECORDING-V1-000001";
    std::uint16_t version = 1;
    std::uint32_t flags = 0;
    std::uint64_t started_unix_ms = 0;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    std::uint32_t repeater_id = 0;
    std::int32_t average_rssi_millidbfs = 0;
    std::int32_t latitude_e7 = 0;
    std::int32_t longitude_e7 = 0;
    std::uint32_t duration_ms = 0;
    std::uint32_t ambe_frame_count = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t payload_crc32 = 0;
    int slot = 1;
    int color_code = 1;
    RecordingMode mode = RecordingMode::DmrRelay;
};

std::string to_string(RecordingMode mode);
std::string format_recording_filename(const RecordingMetadata& metadata);
AmbeBurstFrames extract_ambe_frames(const DmrBurstDibits& burst);
std::array<std::uint8_t, 9> pack_ambe_frame(
    const AmbeFrameDibits& frame);
std::vector<std::uint8_t> serialize_ambe_header(
    const AmbeRecordingHeader& header);
AmbeRecordingHeader parse_ambe_header(
    const std::vector<std::uint8_t>& bytes);
std::string format_ambe_recording_filename(
    const RecordingMetadata& metadata);

} // namespace dmr_rpt
