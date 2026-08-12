// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "dmr_rpt/recording.h"
#include "dmr_rpt/config.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dmr_rpt {

struct RecordingNotice {
    std::string event_type;
    std::string result;
    RecordingMetadata metadata;
    std::filesystem::path path;
    std::string reason;
    std::string error;
    std::uint64_t audio_frames = 0;
    std::uint64_t dropped_frames = 0;
};

struct RecordingRuntimeStats {
    std::uint64_t completed_calls = 0;
    std::uint64_t failed_calls = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t dropped_frames = 0;
};

class AudioRecordingRuntime {
public:
    using NoticeCallback = std::function<void(const RecordingNotice&)>;

    AudioRecordingRuntime(std::filesystem::path output_directory,
                          std::size_t maximum_queued_frames,
                          NoticeCallback notice_callback = {});
    AudioRecordingRuntime(std::filesystem::path output_directory,
                          std::size_t maximum_queued_frames,
                          RemoteVoiceConfig remote_voice_config,
                          NoticeCallback notice_callback = {});
    ~AudioRecordingRuntime();

    AudioRecordingRuntime(const AudioRecordingRuntime&) = delete;
    AudioRecordingRuntime& operator=(const AudioRecordingRuntime&) = delete;

    void start_call(const RecordingMetadata& metadata);
    void submit_dmr_burst(const std::string& correlation_id,
                          const DmrBurstDibits& burst,
                          std::optional<double> rssi_dbfs = {});
    void submit_pcm(RecordingMode mode, const float* samples, std::size_t count);
    void finish_call(const std::string& correlation_id,
                     const std::string& reason);
    void stop() noexcept;
    RecordingRuntimeStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dmr_rpt
