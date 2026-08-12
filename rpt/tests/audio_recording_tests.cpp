// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/audio_recording_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path unique_temp_dir()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("dmr_audio_recording_tests_" + std::to_string(ticks));
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<float> tone(std::size_t count, double frequency_hz)
{
    std::vector<float> samples(count);
    for (std::size_t index = 0; index < count; ++index) {
        samples[index] = static_cast<float>(
            0.25 * std::sin(2.0 * kPi * frequency_hz *
                            static_cast<double>(index) / 8000.0));
    }
    return samples;
}

void test_modes_and_mp3_output(const std::filesystem::path& root)
{
    std::mutex notice_mutex;
    std::vector<dmr_rpt::RecordingNotice> notices;
    dmr_rpt::AudioRecordingRuntime runtime(
        root, 4096,
        [&](const dmr_rpt::RecordingNotice& notice) {
            std::lock_guard<std::mutex> lock(notice_mutex);
            notices.push_back(notice);
        });

    const std::vector<float> pcm = tone(8000U, 1000.0);
    const auto record_pcm_mode = [&](dmr_rpt::RecordingMode mode,
                                     const std::string& correlation) {
        dmr_rpt::RecordingMetadata metadata;
        metadata.mode = mode;
        metadata.source_id = mode == dmr_rpt::RecordingMode::FmRelay
            ? 9999U
            : 100103U;
        metadata.destination_id = mode == dmr_rpt::RecordingMode::FmRelay
            ? 0xFFFFFFU
            : 2001U;
        metadata.color_code = 1;
        metadata.slot = 1;
        metadata.correlation_id = correlation;
        runtime.start_call(metadata);
        for (std::size_t offset = 0; offset < pcm.size(); offset += 160U) {
            runtime.submit_pcm(mode, pcm.data() + offset, 160U);
        }
        runtime.finish_call(correlation, "test_complete");
    };

    record_pcm_mode(dmr_rpt::RecordingMode::FmRelay, "fm-test");
    record_pcm_mode(dmr_rpt::RecordingMode::DmrDirect, "direct-test");

    dmr_rpt::RecordingMetadata dmr;
    dmr.mode = dmr_rpt::RecordingMode::DmrRelay;
    dmr.source_id = 100103;
    dmr.destination_id = 2001;
    dmr.color_code = 1;
    dmr.slot = 2;
    dmr.correlation_id = "dmr-test";
    runtime.start_call(dmr);
    dmr_rpt::DmrBurstDibits burst {};
    for (std::size_t index = 0; index < burst.size(); ++index) {
        burst[index] = static_cast<std::uint8_t>(index % 4U);
    }
    for (unsigned index = 0; index < 20U; ++index) {
        runtime.submit_dmr_burst(dmr.correlation_id, burst);
    }
    runtime.finish_call(dmr.correlation_id, "test_complete");
    runtime.stop();

    const dmr_rpt::RecordingRuntimeStats stats = runtime.stats();
    require(stats.completed_calls == 3U, "all recording modes complete");
    require(stats.failed_calls == 0U, "recording modes have no encoder failure");
    require(stats.dropped_frames == 0U, "recording queue does not drop test audio");

    std::size_t mp3_count = 0;
    bool have_fm = false;
    bool have_direct = false;
    bool have_relay = false;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().extension() != ".mp3") {
            continue;
        }
        ++mp3_count;
        require(entry.file_size() > 0U, "completed MP3 is non-empty");
        const std::string name = entry.path().filename().string();
        have_fm = have_fm || name.find("_fm_relay_") != std::string::npos;
        have_direct = have_direct ||
            name.find("_dmr_direct_") != std::string::npos;
        have_relay = have_relay ||
            name.find("_dmr_relay_") != std::string::npos;
    }
    require(mp3_count == 3U, "one MP3 is finalized for each recording mode");
    require(have_fm && have_direct && have_relay,
            "MP3 filenames identify all recording modes");
    require(std::none_of(
                std::filesystem::directory_iterator(root),
                std::filesystem::directory_iterator(),
                [](const auto& entry) {
                    return entry.path().extension() == ".part";
                }),
            "successful calls leave no partial recording");

    std::lock_guard<std::mutex> lock(notice_mutex);
    require(notices.size() == 6U,
            "each recording produces start and completion notices");
}

void test_failure_isolation(const std::filesystem::path& root)
{
    const std::filesystem::path blocker = root / "not_a_directory";
    {
        std::ofstream stream(blocker);
        stream << "block";
    }

    dmr_rpt::AudioRecordingRuntime runtime(blocker / "recordings", 8);
    dmr_rpt::RecordingMetadata metadata;
    metadata.mode = dmr_rpt::RecordingMode::FmRelay;
    metadata.source_id = 9999;
    metadata.destination_id = 0xFFFFFFU;
    metadata.color_code = 1;
    metadata.slot = 1;
    metadata.correlation_id = "failure-test";
    const std::vector<float> pcm = tone(160U, 500.0);
    runtime.start_call(metadata);
    runtime.submit_pcm(metadata.mode, pcm.data(), pcm.size());
    runtime.finish_call(metadata.correlation_id, "test_complete");
    runtime.stop();
    require(runtime.stats().failed_calls == 1U,
            "recording sink failure is contained in the worker");
}

} // namespace

int main()
{
    const std::filesystem::path root = unique_temp_dir();
    try {
        test_modes_and_mp3_output(root / "recordings");
        test_failure_isolation(root);
        std::filesystem::remove_all(root);
        std::cout << "audio recording tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_recording_tests: " << error.what() << '\n';
        return 1;
    }
}
