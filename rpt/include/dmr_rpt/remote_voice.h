// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "dmr_rpt/config.h"
#include "dmr_rpt/recording.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace dmr_rpt {

class AmbeRecordingWriter {
public:
    AmbeRecordingWriter() = default;
    ~AmbeRecordingWriter();

    AmbeRecordingWriter(const AmbeRecordingWriter&) = delete;
    AmbeRecordingWriter& operator=(const AmbeRecordingWriter&) = delete;

    void start(const RecordingMetadata& metadata,
               const RemoteVoiceConfig& config);
    void observe_rssi(double rssi_dbfs);
    void submit_burst(const DmrBurstDibits& burst);
    std::filesystem::path finish(std::int64_t duration_ms);
    void abort() noexcept;
    bool active() const;

private:
    std::filesystem::path final_path_;
    std::filesystem::path partial_path_;
    std::fstream stream_;
    AmbeRecordingHeader header_;
    std::uint32_t crc_state_ = 0xFFFFFFFFU;
    double rssi_power_sum_ = 0.0;
    std::uint64_t rssi_sample_count_ = 0;
    bool active_ = false;
};

class RemoteVoiceUploader {
public:
    explicit RemoteVoiceUploader(RemoteVoiceConfig config);

    void upload(const std::filesystem::path& file) const;

private:
    RemoteVoiceConfig config_;
};

} // namespace dmr_rpt
