// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/audio_recording_runtime.h"
#include "dmr_rpt/remote_voice.h"

#include <lame/lame.h>

extern "C" {
#include <mbelib.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dmr_rpt {
namespace {

constexpr int kAudioSampleRate = 8000;

std::filesystem::path available_path(const std::filesystem::path& requested)
{
    const std::filesystem::path directory = requested.parent_path();
    const std::string stem = requested.stem().string();
    const std::string extension = requested.extension().string();
    for (unsigned suffix = 0; suffix < 10000U; ++suffix) {
        const std::string suffix_text = suffix == 0U
            ? std::string{}
            : "_" + std::to_string(suffix);
        const std::filesystem::path candidate =
            directory / (stem + suffix_text + extension);
        if (!std::filesystem::exists(candidate) &&
            !std::filesystem::exists(candidate.string() + ".part")) {
            return candidate;
        }
    }
    throw std::runtime_error(
        "cannot allocate unique recording filename: " + requested.string());
}

void place_ambe_dibits(const AmbeFrameDibits& dibits, char frame[4][24])
{
    constexpr std::array<int, 36> r_w {
        0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
        0,1,0,1,0,2,0,2,0,2,0,2,0,2,0,2,0,2
    };
    constexpr std::array<int, 36> r_x {
        23,10,22,9,21,8,20,7,19,6,18,5,17,4,16,3,15,2,
        14,1,13,0,12,10,11,9,10,8,9,7,8,6,7,5,6,4
    };
    constexpr std::array<int, 36> r_y {
        0,2,0,2,0,2,0,2,0,3,0,3,1,3,1,3,1,3,
        1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3
    };
    constexpr std::array<int, 36> r_z {
        5,3,4,2,3,1,2,0,1,13,0,12,22,11,21,10,20,9,
        19,8,18,7,17,6,16,5,15,4,14,3,13,2,12,1,11,0
    };
    std::fill_n(&frame[0][0], 4U * 24U, char{0});
    for (std::size_t index = 0; index < dibits.size(); ++index) {
        frame[r_w[index]][r_x[index]] =
            static_cast<char>((dibits[index] >> 1U) & 1U);
        frame[r_y[index]][r_z[index]] =
            static_cast<char>(dibits[index] & 1U);
    }
}

} // namespace

struct AudioRecordingRuntime::Impl {
    enum class WorkType {
        Start,
        DmrBurst,
        Pcm,
        Finish,
        Shutdown,
    };

    struct WorkItem {
        WorkType type = WorkType::Shutdown;
        RecordingMetadata metadata;
        std::string correlation_id;
        std::string reason;
        std::optional<double> rssi_dbfs;
        DmrBurstDibits burst {};
        std::vector<float> pcm;
    };

    Impl(std::filesystem::path directory,
         std::size_t maximum_frames,
         RemoteVoiceConfig remote_config,
         NoticeCallback callback)
        : output_directory(std::move(directory))
        , maximum_queued_frames(std::max<std::size_t>(1U, maximum_frames))
        , remote_voice_config(std::move(remote_config))
        , notice_callback(std::move(callback))
        , worker([this] { run(); })
    {
    }

    ~Impl()
    {
        stop();
    }

    void notify(RecordingNotice notice) noexcept
    {
        if (!notice_callback) {
            return;
        }
        try {
            notice_callback(notice);
        } catch (...) {
        }
    }

    void enqueue_control(WorkItem item)
    {
        queue.push_back(std::move(item));
        condition.notify_one();
    }

    bool enqueue_audio(WorkItem item)
    {
        if (queued_audio_frames >= maximum_queued_frames) {
            const auto oldest = std::find_if(
                queue.begin(), queue.end(), [](const WorkItem& queued) {
                    return queued.type == WorkType::DmrBurst ||
                           queued.type == WorkType::Pcm;
                });
            if (oldest == queue.end()) {
                ++runtime_stats.dropped_frames;
                return false;
            }
            queue.erase(oldest);
            --queued_audio_frames;
            ++runtime_stats.dropped_frames;
        }
        queue.push_back(std::move(item));
        ++queued_audio_frames;
        condition.notify_one();
        return true;
    }

    void start_call(const RecordingMetadata& metadata)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return;
        }
        producer_session = metadata;
        WorkItem item;
        item.type = WorkType::Start;
        item.metadata = metadata;
        item.correlation_id = metadata.correlation_id;
        enqueue_control(std::move(item));
    }

    void submit_dmr_burst(const std::string& correlation_id,
                          const DmrBurstDibits& burst,
                          std::optional<double> rssi_dbfs)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !producer_session ||
            producer_session->mode != RecordingMode::DmrRelay ||
            producer_session->correlation_id != correlation_id) {
            return;
        }
        WorkItem item;
        item.type = WorkType::DmrBurst;
        item.correlation_id = correlation_id;
        item.rssi_dbfs = rssi_dbfs;
        item.burst = burst;
        enqueue_audio(std::move(item));
    }

    void submit_pcm(RecordingMode mode, const float* samples, std::size_t count)
    {
        if (samples == nullptr || count == 0U) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !producer_session || producer_session->mode != mode) {
            return;
        }
        WorkItem item;
        item.type = WorkType::Pcm;
        item.correlation_id = producer_session->correlation_id;
        item.pcm.assign(samples, samples + count);
        enqueue_audio(std::move(item));
    }

    void finish_call(const std::string& correlation_id,
                     const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return;
        }
        if (producer_session &&
            producer_session->correlation_id == correlation_id) {
            producer_session.reset();
        }
        WorkItem item;
        item.type = WorkType::Finish;
        item.correlation_id = correlation_id;
        item.reason = reason;
        enqueue_control(std::move(item));
    }

    void stop() noexcept
    {
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) {
                    return;
                }
                stopping = true;
                producer_session.reset();
                WorkItem item;
                item.type = WorkType::Shutdown;
                enqueue_control(std::move(item));
            }
            if (worker.joinable()) {
                worker.join();
            }
        } catch (...) {
        }
    }

    RecordingRuntimeStats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return runtime_stats;
    }

    std::uint64_t dropped_frames() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return runtime_stats.dropped_frames;
    }

    void add_audio_frame()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++runtime_stats.audio_frames;
    }

    void add_completed_call()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++runtime_stats.completed_calls;
    }

    void add_failed_call()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++runtime_stats.failed_calls;
    }

    void reset_decoder()
    {
        mbe_initMbeParms(&current, &previous, &enhanced);
        peak_history.fill(0.0F);
        peak_index = 0;
        audio_gain = 25.0F;
    }

    void open_session(const RecordingMetadata& metadata)
    {
        if (active_session) {
            close_session("superseded");
        }
        if (output_directory.empty()) {
            throw std::runtime_error("recording output directory is empty");
        }
        std::filesystem::create_directories(output_directory);
        final_path = available_path(
            output_directory / format_recording_filename(metadata));
        partial_path = final_path.string() + ".part";
        stream.open(partial_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            throw std::runtime_error(
                "cannot create recording file: " + partial_path.string());
        }

        encoder = lame_init();
        if (encoder == nullptr) {
            abort_session();
            throw std::runtime_error("cannot initialize MP3 encoder");
        }
        lame_set_in_samplerate(encoder, kAudioSampleRate);
        lame_set_out_samplerate(encoder, kAudioSampleRate);
        lame_set_num_channels(encoder, 1);
        lame_set_mode(encoder, MONO);
        lame_set_brate(encoder, 16);
        lame_set_quality(encoder, 2);
        if (lame_init_params(encoder) < 0) {
            abort_session();
            throw std::runtime_error("cannot configure MP3 encoder");
        }

        active_session = metadata;
        active_audio_frames = 0;
        reset_decoder();
        if (metadata.mode == RecordingMode::DmrRelay &&
            remote_voice_config.enabled) {
            ambe_writer.start(metadata, remote_voice_config);
        }
        notify({"recording.started", "ok", metadata, final_path, {}, {}, 0,
                dropped_frames()});
    }

    void write_pcm(const float* samples, std::size_t count)
    {
        if (!active_session || encoder == nullptr || count == 0U) {
            return;
        }
        if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("PCM block exceeds MP3 encoder limit");
        }
        std::vector<short> pcm(count);
        for (std::size_t index = 0; index < count; ++index) {
            const float clipped = std::clamp(samples[index], -1.0F, 1.0F);
            pcm[index] = static_cast<short>(std::lrint(clipped * 32767.0F));
        }
        std::vector<unsigned char> encoded(
            static_cast<std::size_t>(std::ceil(1.25 * count)) + 7200U);
        const int bytes = lame_encode_buffer(
            encoder, pcm.data(), pcm.data(), static_cast<int>(count),
            encoded.data(), static_cast<int>(encoded.size()));
        if (bytes < 0) {
            throw std::runtime_error("MP3 encoder rejected PCM data");
        }
        stream.write(reinterpret_cast<const char*>(encoded.data()), bytes);
        if (!stream) {
            throw std::runtime_error(
                "failed to write recording file: " + partial_path.string());
        }
    }

    void decode_frame(const AmbeFrameDibits& dibits,
                      std::vector<float>& audio)
    {
        char frame[4][24] {};
        char data[49] {};
        char error_text[64] {};
        std::array<float, 160> decoded {};
        int errors = 0;
        int corrected_errors = 0;
        place_ambe_dibits(dibits, frame);
        mbe_processAmbe3600x2450Framef(
            decoded.data(), &errors, &corrected_errors, error_text, frame, data,
            &current, &previous, &enhanced, 3);

        float peak = 0.0F;
        for (const float sample : decoded) {
            peak = std::max(peak, std::abs(sample));
        }
        peak_history[peak_index] = peak;
        peak_index = (peak_index + 1U) % peak_history.size();
        const float history_peak = *std::max_element(
            peak_history.begin(), peak_history.end());
        float target_gain = history_peak > 0.0F
            ? 30000.0F / history_peak
            : 50.0F;
        target_gain = std::min(target_gain, 50.0F);
        const float start_gain = audio_gain;
        if (target_gain < audio_gain) {
            audio_gain = target_gain;
        } else {
            audio_gain = std::min(target_gain, audio_gain * 1.05F);
        }
        for (std::size_t index = 0; index < decoded.size(); ++index) {
            const float position = static_cast<float>(index + 1U) /
                static_cast<float>(decoded.size());
            const float gain = start_gain + position * (audio_gain - start_gain);
            audio.push_back(std::clamp(
                decoded[index] * gain / 32768.0F, -1.0F, 1.0F));
        }
    }

    void process_dmr_burst(const WorkItem& item)
    {
        if (!active_session ||
            active_session->mode != RecordingMode::DmrRelay ||
            active_session->correlation_id != item.correlation_id) {
            return;
        }
        const AmbeBurstFrames frames = extract_ambe_frames(item.burst);
        if (ambe_writer.active()) {
            if (item.rssi_dbfs) {
                ambe_writer.observe_rssi(*item.rssi_dbfs);
            }
            ambe_writer.submit_burst(item.burst);
        }
        std::vector<float> audio;
        audio.reserve(480U);
        for (const AmbeFrameDibits& frame : frames) {
            decode_frame(frame, audio);
        }
        write_pcm(audio.data(), audio.size());
        ++active_audio_frames;
        add_audio_frame();
    }

    void process_pcm(const WorkItem& item)
    {
        if (!active_session ||
            active_session->correlation_id != item.correlation_id ||
            (active_session->mode != RecordingMode::FmRelay &&
             active_session->mode != RecordingMode::DmrDirect)) {
            return;
        }
        write_pcm(item.pcm.data(), item.pcm.size());
        ++active_audio_frames;
        add_audio_frame();
    }

    void close_session(const std::string& reason)
    {
        if (!active_session) {
            return;
        }
        const RecordingMetadata metadata = *active_session;
        const std::uint64_t frames = active_audio_frames;
        std::string failure;
        if (encoder != nullptr) {
            std::array<unsigned char, 7200> encoded {};
            const int bytes = lame_encode_flush(
                encoder, encoded.data(), static_cast<int>(encoded.size()));
            if (bytes < 0) {
                failure = "failed to flush MP3 encoder";
            } else {
                stream.write(reinterpret_cast<const char*>(encoded.data()), bytes);
            }
            lame_close(encoder);
            encoder = nullptr;
        }
        stream.flush();
        if (!stream && failure.empty()) {
            failure = "failed to flush recording file";
        }
        stream.close();

        if (failure.empty()) {
            std::error_code rename_error;
            std::filesystem::rename(partial_path, final_path, rename_error);
            if (rename_error) {
                failure = "cannot finalize recording file: " +
                    rename_error.message();
            }
        }

        active_session.reset();
        active_audio_frames = 0;
        if (!failure.empty()) {
            add_failed_call();
            std::error_code remove_error;
            std::filesystem::remove(partial_path, remove_error);
            notify({"recording.failed", "failed", metadata, final_path, reason,
                    failure, frames, dropped_frames()});
            return;
        }
        add_completed_call();
        notify({"recording.completed", "ok", metadata, final_path, reason, {},
                frames, dropped_frames()});
        if (ambe_writer.active()) {
            try {
                const auto duration = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::system_clock::now() - metadata.started_at)
                                          .count();
                const std::filesystem::path ambe_path =
                    ambe_writer.finish(duration);
                RemoteVoiceUploader(remote_voice_config).upload(ambe_path);
                notify({"recording.remote_upload_completed", "ok", metadata,
                        ambe_path, reason, {}, frames, dropped_frames()});
            } catch (const std::exception& error) {
                notify({"recording.remote_upload_failed", "failed", metadata,
                        {}, reason, error.what(), frames, dropped_frames()});
            }
        }
    }

    void abort_session() noexcept
    {
        if (encoder != nullptr) {
            lame_close(encoder);
            encoder = nullptr;
        }
        if (stream.is_open()) {
            stream.close();
        }
        std::error_code error;
        if (!partial_path.empty()) {
            std::filesystem::remove(partial_path, error);
        }
        active_session.reset();
        active_audio_frames = 0;
        ambe_writer.abort();
    }

    void fail_active(const WorkItem& item, const std::string& error)
    {
        RecordingMetadata metadata = active_session
            ? *active_session
            : item.metadata;
        const std::filesystem::path path = final_path;
        const std::uint64_t frames = active_audio_frames;
        abort_session();
        add_failed_call();
        notify({"recording.failed", "failed", metadata, path, item.reason,
                error, frames, dropped_frames()});
    }

    void run() noexcept
    {
        for (;;) {
            WorkItem item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return !queue.empty(); });
                item = std::move(queue.front());
                queue.pop_front();
                if (item.type == WorkType::DmrBurst ||
                    item.type == WorkType::Pcm) {
                    --queued_audio_frames;
                }
            }

            if (item.type == WorkType::Shutdown) {
                try {
                    close_session("shutdown");
                } catch (const std::exception& error) {
                    fail_active(item, error.what());
                }
                return;
            }

            try {
                switch (item.type) {
                case WorkType::Start:
                    open_session(item.metadata);
                    break;
                case WorkType::DmrBurst:
                    process_dmr_burst(item);
                    break;
                case WorkType::Pcm:
                    process_pcm(item);
                    break;
                case WorkType::Finish:
                    if (active_session &&
                        active_session->correlation_id == item.correlation_id) {
                        close_session(item.reason);
                    }
                    break;
                case WorkType::Shutdown:
                    break;
                }
            } catch (const std::exception& error) {
                fail_active(item, error.what());
            } catch (...) {
                fail_active(item, "unknown recording worker failure");
            }
        }
    }

    std::filesystem::path output_directory;
    std::size_t maximum_queued_frames;
    RemoteVoiceConfig remote_voice_config;
    NoticeCallback notice_callback;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<WorkItem> queue;
    std::size_t queued_audio_frames = 0;
    bool stopping = false;
    std::optional<RecordingMetadata> producer_session;
    std::thread worker;
    RecordingRuntimeStats runtime_stats;

    std::optional<RecordingMetadata> active_session;
    std::filesystem::path final_path;
    std::filesystem::path partial_path;
    std::ofstream stream;
    lame_t encoder = nullptr;
    std::uint64_t active_audio_frames = 0;
    mbe_parms current {};
    mbe_parms previous {};
    mbe_parms enhanced {};
    std::array<float, 25> peak_history {};
    std::size_t peak_index = 0;
    float audio_gain = 25.0F;
    AmbeRecordingWriter ambe_writer;
};

AudioRecordingRuntime::AudioRecordingRuntime(
    std::filesystem::path output_directory,
    std::size_t maximum_queued_frames,
    NoticeCallback notice_callback)
    : impl_(std::make_unique<Impl>(
          std::move(output_directory), maximum_queued_frames,
          RemoteVoiceConfig{},
          std::move(notice_callback)))
{
}

AudioRecordingRuntime::AudioRecordingRuntime(
    std::filesystem::path output_directory,
    std::size_t maximum_queued_frames,
    RemoteVoiceConfig remote_voice_config,
    NoticeCallback notice_callback)
    : impl_(std::make_unique<Impl>(
          std::move(output_directory), maximum_queued_frames,
          std::move(remote_voice_config), std::move(notice_callback)))
{
}

AudioRecordingRuntime::~AudioRecordingRuntime()
{
    stop();
}

void AudioRecordingRuntime::start_call(const RecordingMetadata& metadata)
{
    impl_->start_call(metadata);
}

void AudioRecordingRuntime::submit_dmr_burst(
    const std::string& correlation_id,
    const DmrBurstDibits& burst,
    std::optional<double> rssi_dbfs)
{
    impl_->submit_dmr_burst(correlation_id, burst, rssi_dbfs);
}

void AudioRecordingRuntime::submit_pcm(
    RecordingMode mode, const float* samples, std::size_t count)
{
    impl_->submit_pcm(mode, samples, count);
}

void AudioRecordingRuntime::finish_call(
    const std::string& correlation_id, const std::string& reason)
{
    impl_->finish_call(correlation_id, reason);
}

void AudioRecordingRuntime::stop() noexcept
{
    if (impl_) {
        impl_->stop();
    }
}

RecordingRuntimeStats AudioRecordingRuntime::stats() const
{
    return impl_->stats();
}

} // namespace dmr_rpt
