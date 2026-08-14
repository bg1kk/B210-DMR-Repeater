// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/hardware_runtime.h"

#include "dmr_b210/dmr_burst_symbol_sampler.h"
#include "dmr_b210/dmr_direct_frame_builder.h"
#include "dmr_rpt/analog_fm.h"
#include "dmr_rpt/audio_recording_runtime.h"
#include "dmr_rpt/dmr_burst.h"
#include "dmr_rpt/event.h"
#include "dmr_rpt/interlock.h"
#include "dmr_rpt/io_status.h"
#include "dmr_rpt/network_protocol.h"
#include "dmr_rpt/receive_agc.h"
#include "dmr_rpt/receive_signal_metrics.h"
#include "dmr_rpt/router.h"

#include <gnuradio/analog/frequency_modulator_fc.h>
#include <gnuradio/analog/pwr_squelch_cc.h>
#include <gnuradio/analog/quadrature_demod_cf.h>
#include <gnuradio/analog/sig_source.h>
#include <gnuradio/block.h>
#include <gnuradio/blocks/delay.h>
#include <gnuradio/blocks/float_to_complex.h>
#include <gnuradio/blocks/float_to_short.h>
#include <gnuradio/blocks/multiply.h>
#include <gnuradio/blocks/multiply_const.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/repeat.h>
#include <gnuradio/blocks/vector_source.h>
#include <gnuradio/digital/chunks_to_symbols.h>
#include <gnuradio/dmr/frame_decoder.h>
#include <gnuradio/filter/firdes.h>
#include <gnuradio/filter/freq_xlating_fir_filter.h>
#include <gnuradio/filter/fir_filter_blk.h>
#include <gnuradio/filter/interp_fir_filter.h>
#include <gnuradio/gr_complex.h>
#include <gnuradio/io_signature.h>
#include <gnuradio/op25_repeater/ambe_encoder_sb.h>
#include <gnuradio/sync_block.h>
#include <gnuradio/top_block.h>
#include <gnuradio/uhd/usrp_sink.h>
#include <gnuradio/uhd/usrp_source.h>
#include <pmt/pmt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dmr_rpt {
namespace {

constexpr double kSymbolRate = 4800.0;
constexpr double kUsrpRate = 480000.0;
constexpr double kDemodRate = 48000.0;
constexpr double kDmrDeviation = 1944.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kSamplesPerSymbol = 100;
constexpr int kTxRrcTapCount = 1001;
constexpr int kTxRrcDelaySamples = (kTxRrcTapCount - 1) / 2;
constexpr int kBurstDibits = 132;
constexpr int kSlotDibits = 144;
constexpr int kFrameDibits = 288;
constexpr std::size_t kMaximumQueuedBursts = 256;
constexpr std::size_t kMaximumPreAdmissionBursts = 8;
constexpr std::size_t kMaximumQueuedAmbeFrames =
    kAnalogFmMaxQueuedAmbeFrames;
constexpr std::size_t kAmbeFramesPerDmrBurst = 3;
constexpr unsigned kAfmHeaderBursts = kAnalogFmRelayHeaderBursts;
constexpr unsigned kAfmTerminatorBursts = 2;

static_assert(kBurstDibits == static_cast<int>(kDmrBurstDibits));
static_assert(kFrameDibits == kDirectModeFrameDibits);

const char* sync_kind_name(DmrBurstSyncKind kind)
{
    switch (kind) {
    case DmrBurstSyncKind::Data:
        return "data";
    case DmrBurstSyncKind::Voice:
        return "voice";
    case DmrBurstSyncKind::Reverse:
        return "reverse";
    case DmrBurstSyncKind::Unknown:
        break;
    }
    return "unknown";
}

void require_in_range(const ::uhd::meta_range_t& range,
                      double value,
                      double tolerance,
                      const char* label)
{
    if (range.empty() || std::abs(range.clip(value, false) - value) > tolerance) {
        throw std::runtime_error(std::string(label) + " is outside UHD capability range");
    }
}

void require_precision(double requested,
                       double applied,
                       double tolerance,
                       const char* label)
{
    if (std::abs(requested - applied) > tolerance) {
        std::ostringstream message;
        message << label << " precision not met: requested=" << requested
                << ", applied=" << applied;
        throw std::runtime_error(message.str());
    }
}

std::int64_t monotonic_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<long> dict_long(const pmt::pmt_t& dict, const char* key)
{
    const pmt::pmt_t value = pmt::dict_ref(dict, pmt::mp(key), pmt::PMT_NIL);
    return pmt::is_integer(value) ? std::optional<long>(pmt::to_long(value))
                                  : std::nullopt;
}

bool dict_bool(const pmt::pmt_t& dict, const char* key)
{
    const pmt::pmt_t value = pmt::dict_ref(dict, pmt::mp(key), pmt::PMT_NIL);
    return pmt::is_bool(value) && pmt::to_bool(value);
}

std::string dict_symbol(const pmt::pmt_t& dict, const char* key)
{
    const pmt::pmt_t value = pmt::dict_ref(dict, pmt::mp(key), pmt::PMT_NIL);
    return pmt::is_symbol(value) ? pmt::symbol_to_string(value) : std::string{};
}

std::vector<std::uint8_t> dict_u8vector(const pmt::pmt_t& dict, const char* key)
{
    const pmt::pmt_t value = pmt::dict_ref(dict, pmt::mp(key), pmt::PMT_NIL);
    return pmt::is_u8vector(value) ? pmt::u8vector_elements(value)
                                   : std::vector<std::uint8_t>{};
}

CallType call_type_from_metadata(long value, std::uint32_t destination_id)
{
    if (destination_id == 0xFFFFFFU) {
        return CallType::AllCall;
    }
    return value == 1 ? CallType::Private : CallType::Group;
}

const char* receive_call_mode(CallType call_type)
{
    switch (call_type) {
    case CallType::Private:
        return "private";
    case CallType::Group:
        return "group";
    case CallType::AllCall:
        return "all_call";
    default:
        return "unknown";
    }
}

const AnalogFmFallbackConfig& active_afm(const RepeaterConfig& config)
{
    const auto profile = std::find_if(
        config.channel_profiles.begin(), config.channel_profiles.end(),
        [&](const ChannelProfile& item) {
            return item.id == config.radio.active_channel_profile_id;
        });
    if (profile == config.channel_profiles.end()) {
        throw std::runtime_error("active channel profile is unavailable");
    }
    return profile->analog_fm_fallback;
}

std::string format_decimal(double value)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << value;
    return output.str();
}

std::string format_receive_signal_status(
    const char* mode,
    const std::shared_ptr<ReceiveSignalMetrics>& metrics)
{
    const ReceiveSignalSnapshot snapshot = metrics
        ? metrics->snapshot()
        : ReceiveSignalSnapshot{};
    std::ostringstream output;
    output << mode << " RX Rssi=";
    if (snapshot.signal_dbfs) {
        output << format_decimal(*snapshot.signal_dbfs) << "dBFS";
    } else {
        output << "--.-dBFS";
    }
    output << " SNR=";
    if (snapshot.snr_db) {
        output << format_decimal(*snapshot.snr_db) << "dB";
    } else {
        output << "--.-dB";
    }
    return output.str();
}

class GnuradioB210GpioAdapter final : public B210GpioAdapter {
public:
    explicit GnuradioB210GpioAdapter(gr::uhd::usrp_source::sptr source)
        : source_(std::move(source))
    {
    }

    GpioCapability capability(const std::string& bank) override
    {
        GpioCapability result;
        result.bank = bank;
        const std::vector<std::string> banks = source_->get_gpio_banks(0);
        if (std::find(banks.begin(), banks.end(), bank) == banks.end()) {
            return result;
        }
        for (int pin = 0; pin < 32; ++pin) {
            result.available_pins.push_back(pin);
            result.output_capable_pins.push_back(pin);
        }
        return result;
    }

    bool configure_output(const std::string& bank, int pin) override
    {
        try {
            const std::uint32_t mask = std::uint32_t{1} << pin;
            source_->set_gpio_attr(bank, "CTRL", 0, mask, 0);
            source_->set_gpio_attr(bank, "DDR", mask, mask, 0);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool write(const std::string& bank, int pin, IoLevel level) override
    {
        try {
            const std::uint32_t mask = std::uint32_t{1} << pin;
            const std::uint32_t value = level == IoLevel::High ? mask : 0U;
            source_->set_gpio_attr(bank, "OUT", value, mask, 0);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

private:
    gr::uhd::usrp_source::sptr source_;
};

class RuntimeIo {
public:
    RuntimeIo(IoStatusConfig config, B210GpioAdapter& adapter)
        : controller_(std::move(config), adapter)
    {
    }

    void initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_.initialize(monotonic_ms());
    }

    void rx(int channel, bool active)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_.on_rx_activity(channel, active, active, monotonic_ms());
    }

    void tx(int channel, bool active)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_.on_tx_ptt(channel, active, monotonic_ms());
    }

    void poll(std::int64_t now_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_.poll(now_ms);
    }

    void release()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        controller_.release_all_high();
    }

private:
    std::mutex mutex_;
    B210IoStatusController controller_;
};

class CtcssMonitorBlock final : public gr::sync_block {
public:
    using sptr = std::shared_ptr<CtcssMonitorBlock>;
    using StateCallback = std::function<void(const CtcssState&)>;

    static sptr make(const CtcssConfig& config, StateCallback callback)
    {
        return gnuradio::make_block_sptr<CtcssMonitorBlock>(
            config, std::move(callback));
    }

    CtcssMonitorBlock(const CtcssConfig& config, StateCallback callback)
        : gr::sync_block("ctcss_monitor",
                         gr::io_signature::make(1, 1, sizeof(float)),
                         gr::io_signature::make(0, 0, 0))
        , detector_(config, 8000)
        , callback_(std::move(callback))
    {
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star&) override
    {
        const auto* input = static_cast<const float*>(input_items[0]);
        const CtcssState state = detector_.process(
            input, static_cast<std::size_t>(noutput_items));
        samples_since_report_ += noutput_items;
        if (!have_reported_ || state.qualified != last_qualified_ ||
            samples_since_report_ >= kReportIntervalSamples) {
            have_reported_ = true;
            last_qualified_ = state.qualified;
            samples_since_report_ = 0;
            if (callback_) {
                callback_(state);
            }
        }
        return noutput_items;
    }

private:
    CtcssDetector detector_;
    StateCallback callback_;
    bool have_reported_ = false;
    bool last_qualified_ = false;
    int samples_since_report_ = 0;
    static constexpr int kReportIntervalSamples = 8000;
};

class RecordingPcmSink final : public gr::sync_block {
public:
    using sptr = std::shared_ptr<RecordingPcmSink>;

    static sptr make(std::shared_ptr<AudioRecordingRuntime> recording)
    {
        return std::make_shared<RecordingPcmSink>(std::move(recording));
    }

    explicit RecordingPcmSink(
        std::shared_ptr<AudioRecordingRuntime> recording)
        : gr::sync_block("dmr_recording_pcm_sink",
                         gr::io_signature::make(1, 1, sizeof(float)),
                         gr::io_signature::make(0, 0, 0))
        , recording_(std::move(recording))
    {
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star&) override
    {
        const auto* input = static_cast<const float*>(input_items[0]);
        recording_->submit_pcm(
            RecordingMode::FmRelay, input,
            static_cast<std::size_t>(noutput_items));
        return noutput_items;
    }

private:
    std::shared_ptr<AudioRecordingRuntime> recording_;
};

class ReceiveSignalMonitor final : public gr::sync_block {
public:
    using sptr = std::shared_ptr<ReceiveSignalMonitor>;
    using SignalCallback = std::function<void(double)>;

    static sptr make(const std::string& name, double threshold_dbfs,
                     std::shared_ptr<ReceiveSignalMetrics> metrics,
                     bool update_console_power, bool verbose,
                     SignalCallback signal_callback = {})
    {
        return std::make_shared<ReceiveSignalMonitor>(
            name, threshold_dbfs, std::move(metrics), update_console_power,
            verbose, std::move(signal_callback));
    }

    ReceiveSignalMonitor(const std::string& name, double threshold_dbfs,
                         std::shared_ptr<ReceiveSignalMetrics> metrics,
                         bool update_console_power, bool verbose,
                         SignalCallback signal_callback)
        : gr::sync_block(name,
                         gr::io_signature::make(1, 1, sizeof(gr_complex)),
                         gr::io_signature::make(1, 1, sizeof(gr_complex)))
        , threshold_dbfs_(threshold_dbfs), metrics_(std::move(metrics))
        , update_console_power_(update_console_power), verbose_(verbose)
        , signal_callback_(std::move(signal_callback))
    {
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override
    {
        const auto* input = static_cast<const gr_complex*>(input_items[0]);
        auto* output = static_cast<gr_complex*>(output_items[0]);
        std::copy_n(input, noutput_items, output);

        constexpr double alpha = 1.0 / (0.2 * kDemodRate);
        constexpr int telemetry_samples = static_cast<int>(kDemodRate / 100.0);
        double telemetry_power = 0.0;
        int telemetry_count = 0;
        for (int index = 0; index < noutput_items; ++index) {
            const double power = std::norm(input[index]);
            average_power_ += alpha * (power - average_power_);
            telemetry_power += power;
            if (++telemetry_count == telemetry_samples) {
                observe_telemetry(telemetry_power / telemetry_count);
                telemetry_power = 0.0;
                telemetry_count = 0;
            }
        }
        if (telemetry_count > 0) {
            observe_telemetry(telemetry_power / telemetry_count);
        }
        samples_since_status_ += static_cast<std::uint64_t>(noutput_items);
        if (verbose_ &&
            samples_since_status_ >= static_cast<std::uint64_t>(kDemodRate)) {
            samples_since_status_ %= static_cast<std::uint64_t>(kDemodRate);
            const double signal_dbfs = 10.0 * std::log10(
                std::max(average_power_, 1e-20));
            write_console_message(
                std::cout,
                std::string("RX ") +
                    (signal_dbfs >= threshold_dbfs_
                         ? console_token::SquelchOpen
                         : console_token::SquelchClosed));
        }
        return noutput_items;
    }

private:
    void observe_telemetry(double power)
    {
        if (metrics_) {
            metrics_->observe_average_power(power);
        }
        if (signal_callback_) {
            signal_callback_(power);
        }
        if (update_console_power_) {
            update_console_rx_power(power);
        }
    }

    double threshold_dbfs_ = -74.0;
    std::shared_ptr<ReceiveSignalMetrics> metrics_;
    double average_power_ = 0.0;
    std::uint64_t samples_since_status_ = 0;
    bool update_console_power_ = false;
    bool verbose_ = false;
    SignalCallback signal_callback_;
};

struct ReceiveAgcTelemetry {
    std::atomic<int> input_tenths_dbfs{-2000};
    std::atomic<int> gain_tenths_db{0};
};

class ReceiveAgcBlock final : public gr::sync_block {
public:
    using sptr = std::shared_ptr<ReceiveAgcBlock>;

    static sptr make(const std::string& name, const ReceiveAgcConfig& config,
                     double activation_threshold_dbfs,
                     std::shared_ptr<ReceiveAgcTelemetry> telemetry)
    {
        return std::make_shared<ReceiveAgcBlock>(
            name, config, activation_threshold_dbfs, std::move(telemetry));
    }

    ReceiveAgcBlock(const std::string& name, const ReceiveAgcConfig& config,
                    double activation_threshold_dbfs,
                    std::shared_ptr<ReceiveAgcTelemetry> telemetry)
        : gr::sync_block(name,
                         gr::io_signature::make(1, 1, sizeof(gr_complex)),
                         gr::io_signature::make(1, 1, sizeof(gr_complex)))
        , enabled_(config.enabled)
        , controller_(config)
        , activation_threshold_dbfs_(activation_threshold_dbfs)
        , telemetry_(std::move(telemetry))
    {
    }

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override
    {
        const auto* input = static_cast<const gr_complex*>(input_items[0]);
        auto* output = static_cast<gr_complex*>(output_items[0]);
        if (!enabled_) {
            std::copy_n(input, noutput_items, output);
            return noutput_items;
        }

        for (int index = 0; index < noutput_items; ++index) {
            output[index] = input[index] * gain_linear_;
            const double power = std::norm(input[index]);
            if (power > kSilencePower) {
                pending_power_ += power;
                ++pending_samples_;
            }
            if (++window_samples_ == kWindowSamples) {
                update_gain();
                window_samples_ = 0;
            }
        }
        return noutput_items;
    }

private:
    void update_gain()
    {
        if (pending_samples_ == 0) {
            controller_.reset();
            gain_linear_ = 1.0F;
            if (telemetry_) {
                telemetry_->input_tenths_dbfs.store(-2000,
                                                    std::memory_order_relaxed);
                telemetry_->gain_tenths_db.store(0,
                                                 std::memory_order_relaxed);
            }
            return;
        }
        const double average_power = pending_power_ / pending_samples_;
        pending_power_ = 0.0;
        pending_samples_ = 0;
        controller_.observe_average_power(
            average_power, 0.01, activation_threshold_dbfs_);
        const ReceiveAgcSnapshot snapshot = controller_.snapshot();
        gain_linear_ = static_cast<float>(controller_.gain_linear());
        if (telemetry_) {
            telemetry_->input_tenths_dbfs.store(
                snapshot.input_dbfs
                    ? static_cast<int>(std::lround(*snapshot.input_dbfs * 10.0))
                    : -2000,
                std::memory_order_relaxed);
            telemetry_->gain_tenths_db.store(
                static_cast<int>(std::lround(snapshot.gain_db * 10.0)),
                std::memory_order_relaxed);
        }
    }

    static constexpr int kWindowSamples = static_cast<int>(kDemodRate / 100.0);
    static constexpr double kSilencePower = 1e-16;
    bool enabled_ = true;
    ReceiveAgcController controller_;
    double activation_threshold_dbfs_ = -74.0;
    float gain_linear_ = 1.0F;
    double pending_power_ = 0.0;
    int pending_samples_ = 0;
    int window_samples_ = 0;
    std::shared_ptr<ReceiveAgcTelemetry> telemetry_;
};

class DirectRelayBurstSource final : public gr::block {
public:
    using sptr = std::shared_ptr<DirectRelayBurstSource>;
    using ActivityCallback = std::function<void(bool)>;
    using RelayEventCallback = std::function<void(const NetworkRelayEvent&)>;
    using ReceiveCallCallback = std::function<void(
        RelaySource, bool, std::uint32_t, std::uint32_t, CallType)>;

    static sptr make(const RepeaterConfig& config,
                     OperationAuditLogger& audit,
                     std::shared_ptr<AudioRecordingRuntime> recording,
                     std::shared_ptr<ReceiveSignalMetrics> dmr_receive_metrics,
                     std::shared_ptr<ReceiveSignalMetrics> fm_receive_metrics,
                     ActivityCallback dmr_rx_activity,
                     ActivityCallback afm_rx_activity,
                     ActivityCallback tx_activity,
                     bool rx_diagnostic,
                     RelayEventCallback relay_event = {},
                     ReceiveCallCallback receive_call = {},
                     std::shared_ptr<std::atomic_bool> forwarding_enabled = {})
    {
        return std::make_shared<DirectRelayBurstSource>(
            config, audit, std::move(recording),
            std::move(dmr_receive_metrics), std::move(fm_receive_metrics),
            std::move(dmr_rx_activity),
            std::move(afm_rx_activity), std::move(tx_activity),
            rx_diagnostic, std::move(relay_event),
            std::move(receive_call),
            std::move(forwarding_enabled));
    }

    DirectRelayBurstSource(const RepeaterConfig& config,
                           OperationAuditLogger& audit,
                           std::shared_ptr<AudioRecordingRuntime> recording,
                           std::shared_ptr<ReceiveSignalMetrics> dmr_receive_metrics,
                           std::shared_ptr<ReceiveSignalMetrics> fm_receive_metrics,
                           ActivityCallback dmr_rx_activity,
                           ActivityCallback afm_rx_activity,
                           ActivityCallback tx_activity,
                           bool rx_diagnostic,
                           RelayEventCallback relay_event,
                           ReceiveCallCallback receive_call,
                           std::shared_ptr<std::atomic_bool> forwarding_enabled)
        : gr::block("dmr_direct_relay_burst_source",
                    active_afm(config).enabled
                        ? gr::io_signature::makev(
                              2, 2,
                              {dmr_b210::DirectFrameBuilder::kAmbeDibits,
                               sizeof(float)})
                        : gr::io_signature::make(1, 1, sizeof(float)),
                    gr::io_signature::makev(
                        2, 2, {sizeof(std::uint8_t), sizeof(float)}))
        , config_(config)
        , audit_(audit)
        , recording_(std::move(recording))
        , dmr_receive_metrics_(std::move(dmr_receive_metrics))
        , fm_receive_metrics_(std::move(fm_receive_metrics))
        , dmr_admission_tracker_(
              config.dmr.receive_squelch_tenths_dbfs / 10.0,
              config.dmr.receive_inactivity_timeout_ms)
        , router_(config.routing, config.transmit,
                  [this](std::uint32_t source_id) {
                      return cooldowns_.check(source_id, monotonic_ms());
                  })
        , gate_(config.transmit)
        , afm_config_(active_afm(config))
        , startup_prefill_frames_remaining_(kSingleStreamTxStartupPrefillFrames)
        , tx_pipeline_lead_ms_(kSingleStreamTxStartupPrefillFrames *
                               kDirectModeFrameDurationMs)
        , afm_builder_(afm_config_.dmr_tx.source_id,
                       afm_config_.dmr_tx.destination_id,
                       static_cast<unsigned>(afm_config_.dmr_tx.color_code),
                       static_cast<unsigned>(afm_config_.dmr_tx.slot))
        , dmr_rx_activity_(std::move(dmr_rx_activity))
        , afm_rx_activity_(std::move(afm_rx_activity))
        , tx_activity_(std::move(tx_activity))
        , relay_event_(std::move(relay_event))
        , receive_call_(std::move(receive_call))
        , forwarding_enabled_(std::move(forwarding_enabled))
        , rx_diagnostic_(rx_diagnostic)
        , dmr_idle_since_ms_(monotonic_ms())
    {
        message_port_register_in(pmt::mp("frames"));
        set_msg_handler(pmt::mp("frames"), [this](const pmt::pmt_t& message) {
            handle_frame(message);
        });
        message_port_register_in(pmt::mp("bursts"));
        set_msg_handler(pmt::mp("bursts"), [this](const pmt::pmt_t& message) {
            handle_raw_burst(message);
        });
        set_output_multiple(kFrameDibits);
        set_max_noutput_items(kFrameDibits);
        set_min_output_buffer(0, kFrameDibits * kDirectRelayTxOutputBufferFrames);
        set_min_output_buffer(1, kFrameDibits * kDirectRelayTxOutputBufferFrames);
    }

    void forecast(int noutput_items, gr_vector_int& required) override
    {
        std::fill(required.begin(), required.end(), 0);
        if (afm_config_.enabled && !required.empty()) {
            required[0] = 0;
        }
        if (!required.empty()) {
            required[afm_config_.enabled ? 1U : 0U] =
                direct_relay_clock_samples_required(
                    noutput_items, startup_prefill_frames_remaining_);
        }
    }

    void set_afm_ctcss_state(const CtcssState& state)
    {
        const std::int64_t now_ms = monotonic_ms();
        bool changed = false;
        bool reported_qualified = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state.qualified &&
                (active_call_ || dmr_candidate_active_locked(now_ms))) {
                afm_rearm_required_ = true;
            }
            const bool effective_qualified = analog_fm_can_qualify(
                state.qualified, afm_rearm_required_,
                active_call_ || dmr_candidate_active_locked(now_ms));
            changed = afm_ctcss_qualified_ != effective_qualified;
            afm_ctcss_qualified_ = effective_qualified;
            reported_qualified = effective_qualified;
            afm_ctcss_confidence_db_ = state.confidence_db;
            if (effective_qualified && changed) {
                afm_ctcss_qualified_since_ms_ = now_ms;
            }
            if (!effective_qualified) {
                afm_ctcss_qualified_since_ms_ = 0;
                if (!state.qualified) {
                    afm_rearm_required_ = false;
                }
                if (afm_session_ && afm_session_->stage != AfmStage::Terminator) {
                    afm_session_->stage = AfmStage::Terminator;
                    afm_session_->stage_count = 0;
                    afm_session_->end_reason = "ctcss_lost";
                    afm_ambe_queue_.clear();
                }
            }
        }
        if (changed) {
            if (afm_rx_activity_) {
                afm_rx_activity_(reported_qualified);
            }
            report_receive_call(
                RelaySource::Fm, reported_qualified,
                afm_config_.dmr_tx.source_id,
                afm_config_.dmr_tx.destination_id,
                afm_config_.dmr_tx.call_type);
            if (reported_qualified) {
                std::lock_guard<std::mutex> console_lock(console_mutex_);
                write_console_message(
                    std::cout,
                    format_receive_signal_status("FM", fm_receive_metrics_));
            }
            audit_.emit({"AFM", "afm.ctcss_changed", "detect_ctcss",
                         reported_qualified ? "qualified" : "released", "",
                         {{"configured_tone_hz", format_decimal(
                               state.configured_tone_hz)},
                          {"confidence_db", format_decimal(state.confidence_db)}}});
        } else if (now_ms >= next_afm_ctcss_status_at_ms_) {
            next_afm_ctcss_status_at_ms_ = now_ms + 1000;
            const auto snapshot = fm_receive_metrics_
                ? fm_receive_metrics_->snapshot()
                : ReceiveSignalSnapshot{};
            const bool above_threshold = snapshot.signal_dbfs &&
                *snapshot.signal_dbfs >=
                    (fm_receive_metrics_
                         ? fm_receive_metrics_->activity_threshold_dbfs()
                         : -74.0);
            if (above_threshold || state.confidence_db > 1.0) {
                audit_.emit({"AFM", "afm.ctcss_status", "detect_ctcss",
                             state.qualified ? "qualified" : "searching", "",
                             {{"configured_tone_hz", format_decimal(
                                   state.configured_tone_hz)},
                              {"confidence_db", format_decimal(
                                   state.confidence_db)},
                              {"signal_dbfs", snapshot.signal_dbfs
                                   ? format_decimal(*snapshot.signal_dbfs)
                                   : ""},
                              {"threshold_dbfs", fm_receive_metrics_
                                   ? format_decimal(
                                         fm_receive_metrics_
                                             ->activity_threshold_dbfs())
                                   : ""}}});
            }
        }
    }

    void observe_dmr_signal_power(double power)
    {
        const std::int64_t now_ms = monotonic_ms();
        dmr_admission_tracker_.observe_average_power(power, now_ms);
        const double signal_dbfs =
            10.0 * std::log10(std::max(power, 1e-20));
        if (!dmr_receive_metrics_ ||
            signal_dbfs < dmr_receive_metrics_->activity_threshold_dbfs()) {
            return;
        }
        const std::optional<DmrAdmissionTimeoutReason> timeout =
            dmr_admission_tracker_.poll(now_ms);
        if (!timeout) {
            return;
        }
        if (*timeout == DmrAdmissionTimeoutReason::NoReliableSync) {
            emit_rejection("no_sync", "RF_SIGNAL", "SYNC", true, true);
        } else {
            emit_rejection("no_lc", "RF_SIGNAL", "LC", true, true);
        }
    }

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override
    {
        auto* output = static_cast<std::uint8_t*>(output_items[0]);
        auto* gate = static_cast<float*>(output_items[1]);
        const int frame_capacity = std::min(1, noutput_items / kFrameDibits);
        const bool startup_prefill = startup_prefill_frames_remaining_ > 0;
        int produced_frames = 0;
        bool assert_tx = false;
        bool release_tx = false;
        std::vector<ActiveCall> relay_starts;
        std::vector<ActiveCall> afm_starts;
        std::vector<EndedCall> afm_ends;

        int consumed_ambe = 0;
        if (afm_config_.enabled && !ninput_items.empty()) {
            const auto* ambe = static_cast<const std::uint8_t*>(input_items[0]);
            consumed_ambe = ninput_items[0];
            std::lock_guard<std::mutex> lock(mutex_);
            for (int index = 0; index < consumed_ambe; ++index) {
                if (!afm_ctcss_qualified_ && !afm_session_) {
                    continue;
                }
                std::array<std::uint8_t,
                           dmr_b210::DirectFrameBuilder::kAmbeDibits> frame{};
                std::copy_n(
                    ambe + index * dmr_b210::DirectFrameBuilder::kAmbeDibits,
                    frame.size(), frame.begin());
                if (afm_ambe_queue_.size() >= kMaximumQueuedAmbeFrames) {
                    afm_ambe_queue_.pop_front();
                    ++afm_dropped_ambe_frames_;
                }
                afm_ambe_queue_.push_back(frame);
            }
        }

        for (int frame = 0; frame < frame_capacity; ++frame) {
            std::uint8_t* frame_output = output + frame * kFrameDibits;
            float* frame_gate = gate + frame * kFrameDibits;
            std::fill_n(frame_output, kFrameDibits, std::uint8_t{0});
            std::fill_n(frame_gate, kFrameDibits, 0.0F);

            std::optional<QueuedBurst> next;
            bool afm_produced = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const std::int64_t now_ms = monotonic_ms();
                refresh_dmr_idle_locked(now_ms);
                enforce_duration_limit_locked(now_ms);
                enforce_inactivity_timeout_locked(now_ms);
                if ((active_call_ ||
                     dmr_candidate_active_locked(now_ms)) &&
                    afm_session_) {
                    finish_afm_locked(now_ms, "dmr_activity", afm_ends);
                }
                if (!queue_.empty()) {
                    next = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (!next && !active_call_) {
                    afm_produced = produce_afm_locked(
                        frame_output, frame_gate, now_ms, afm_starts, afm_ends);
                }
                if ((next || afm_produced) && !tx_asserted_) {
                    tx_asserted_ = true;
                    assert_tx = true;
                }
                if (next && next->call_start) {
                    next->call_start->relay_started_at_ms = now_ms;
                    if (active_call_ &&
                        active_call_->correlation_id ==
                            next->call_start->correlation_id) {
                        active_call_->relay_started_at_ms = now_ms;
                    }
                    relay_starts.push_back(*next->call_start);
                }
                if (!active_call_ && queue_.empty() &&
                    !afm_session_ &&
                    tx_asserted_) {
                    tx_asserted_ = false;
                    release_tx = true;
                }
            }

            if (next) {
                const int start = next->slot == 2 ? kSlotDibits : 0;
                std::copy(next->dibits.begin(), next->dibits.end(),
                          frame_output + start);
                std::fill_n(frame_gate + start, kBurstDibits, 1.0F);
                ++transmitted_bursts_;
            }
            ++produced_frames;
        }

        if (afm_config_.enabled) {
            consume(0, consumed_ambe);
        }
        if (produced_frames > 0) {
            if (startup_prefill) {
                startup_prefill_frames_remaining_ = std::max(
                    0, startup_prefill_frames_remaining_ - produced_frames);
            } else {
                consume(afm_config_.enabled ? 1 : 0,
                        direct_relay_clock_samples_required(
                            produced_frames * kFrameDibits, 0));
            }
        }

        if (assert_tx && tx_activity_) {
            tx_activity_(true);
        }
        for (const ActiveCall& call : relay_starts) {
            print_call_event("RELAY START", call);
            const std::int64_t enqueue_latency_ms = std::max<std::int64_t>(
                0, call.relay_started_at_ms - call.started_at_ms);
            const std::int64_t estimated_air_start_ms =
                call.relay_started_at_ms + tx_pipeline_lead_ms_;
            const std::int64_t latency_ms = std::max<std::int64_t>(
                0, estimated_air_start_ms - call.started_at_ms);
            const bool latency_ok = relay_start_latency_within_limit(
                call.started_at_ms, estimated_air_start_ms);
            audit_.emit({"RPT", "dmr_relay.started", "transmit_call",
                         latency_ok ? "ok" : "late",
                         call.correlation_id,
                         {{"source_id", std::to_string(call.source_id)},
                          {"destination_id", std::to_string(call.destination_id)},
                          {"call_type", to_string(call.call_type)},
                          {"slot", std::to_string(call.slot)},
                           {"color_code", std::to_string(call.color_code)},
                           {"relay_enqueue_latency_ms", std::to_string(
                                enqueue_latency_ms)},
                           {"tx_pipeline_lead_ms", std::to_string(
                                tx_pipeline_lead_ms_)},
                           {"relay_start_latency_ms", std::to_string(latency_ms)},
                          {"relay_start_latency_limit_ms", std::to_string(
                               kMaximumRelayStartLatencyMs)}}});
            publish_relay_event("started", RelaySource::Dmr, call, 0);
        }
        for (const ActiveCall& call : afm_starts) {
            recording_->start_call(recording_metadata(
                call, RecordingMode::FmRelay));
            print_call_event("CALL START", call);
            print_call_event("RELAY START", call);
            audit_.emit({"AFM", "afm_relay.started", "transmit_call", "ok",
                         call.correlation_id,
                         {{"source_id", std::to_string(call.source_id)},
                          {"destination_id", std::to_string(call.destination_id)},
                          {"call_type", to_string(call.call_type)},
                          {"slot", std::to_string(call.slot)},
                          {"color_code", std::to_string(call.color_code)},
                          {"ctcss_hz", format_decimal(
                               afm_config_.ctcss.tone_tenths_hz / 10.0)}}});
            publish_relay_event("started", RelaySource::Fm, call, 0);
        }
        for (const EndedCall& ended : afm_ends) {
            recording_->finish_call(
                ended.call.correlation_id, ended.reason);
            print_call_event("CALL END", ended.call, ended.reason.c_str(),
                             ended.duration_ms);
            audit_.emit({"AFM", "afm_relay.ended", "transmit_call", "ok",
                         ended.call.correlation_id,
                         {{"source_id", std::to_string(ended.call.source_id)},
                          {"destination_id", std::to_string(
                               ended.call.destination_id)},
                          {"reason", ended.reason},
                          {"relay_duration_ms", std::to_string(
                               ended.duration_ms)},
                          {"relay_duration_hms", format_duration_hms(
                               ended.duration_ms)}}});
            publish_relay_event(
                "ended", RelaySource::Fm, ended.call, ended.duration_ms);
        }
        if (release_tx && tx_activity_) {
            tx_activity_(false);
        }
        return produced_frames * kFrameDibits;
    }

    std::uint64_t accepted_bursts() const { return accepted_bursts_.load(); }
    std::uint64_t transmitted_bursts() const { return transmitted_bursts_.load(); }
    std::uint64_t rejected_frames() const { return rejected_frames_.load(); }
    std::uint64_t analog_fm_sessions() const { return afm_sessions_.load(); }
    std::uint64_t analog_fm_voice_bursts() const
    {
        return afm_voice_bursts_.load();
    }
    std::uint64_t analog_fm_dropped_ambe_frames() const
    {
        return afm_dropped_ambe_frames_.load();
    }

private:
    struct ActiveCall {
        std::uint32_t source_id = 0;
        std::uint32_t destination_id = 0;
        CallType call_type = CallType::Unknown;
        int slot = 1;
        int color_code = 1;
        std::int64_t started_at_ms = 0;
        std::int64_t last_valid_burst_at_ms = 0;
        std::int64_t relay_started_at_ms = 0;
        std::int64_t expires_at_ms = 0;
        std::string correlation_id;
    };

    enum class AfmStage {
        Header,
        Voice,
        Terminator,
    };

    struct AfmSession {
        ActiveCall call;
        AfmStage stage = AfmStage::Header;
        unsigned stage_count = 0;
        unsigned voice_index = 0;
        unsigned underflow_frames = 0;
        std::string end_reason;
    };


    struct EndedCall {
        ActiveCall call;
        std::string reason;
        std::int64_t duration_ms = 0;
    };

    struct QueuedBurst {
        std::array<std::uint8_t, kBurstDibits> dibits{};
        int slot = 1;
        std::optional<ActiveCall> call_start;
    };

    struct PreAdmissionBurst {
        RawDmrBurst dibits{};
        DmrBurstSyncObservation sync;
        std::int64_t received_at_ms = 0;
    };

    RecordingMetadata recording_metadata(
        const ActiveCall& call, RecordingMode mode)
    {
        RecordingMetadata metadata;
        metadata.mode = mode;
        metadata.source_id = call.source_id;
        metadata.destination_id = call.destination_id;
        metadata.color_code = call.color_code;
        metadata.slot = call.slot;
        metadata.correlation_id = call.correlation_id;
        metadata.repeater_id = config_.dmr.repeater_id;
        metadata.latitude_e7 = config_.remote_voice.latitude_e7;
        metadata.longitude_e7 = config_.remote_voice.longitude_e7;
        const std::shared_ptr<ReceiveSignalMetrics>& metrics =
            mode == RecordingMode::FmRelay
                ? fm_receive_metrics_
                : dmr_receive_metrics_;
        if (metrics && metrics->snapshot().signal_dbfs) {
            metadata.average_rssi_dbfs = *metrics->snapshot().signal_dbfs;
        }
        metadata.started_at = std::chrono::system_clock::now();
        return metadata;
    }

    void publish_relay_event(const char* event,
                             RelaySource source,
                             const ActiveCall& call,
                             std::int64_t duration_ms)
    {
        if (!relay_event_) {
            return;
        }
        const std::shared_ptr<ReceiveSignalMetrics>& metrics =
            source == RelaySource::Fm
                ? fm_receive_metrics_
                : dmr_receive_metrics_;
        const std::optional<double> signal = metrics
            ? metrics->snapshot().signal_dbfs
            : std::nullopt;
        relay_event_({
            event,
            source,
            call.source_id,
            call.destination_id,
            call.slot,
            call.color_code,
            duration_ms,
            signal,
            call.correlation_id});
    }

    void report_receive_call(RelaySource source,
                             bool receiving,
                             std::uint32_t source_id = 0,
                             std::uint32_t destination_id = 0,
                             CallType call_type = CallType::Unknown)
    {
        if (receive_call_) {
            receive_call_(source, receiving, source_id, destination_id,
                          call_type);
        }
    }

    bool start_afm_locked(std::int64_t now_ms,
                          std::vector<ActiveCall>& starts)
    {
        if (forwarding_enabled_ &&
            !forwarding_enabled_->load(std::memory_order_relaxed)) {
            return false;
        }
        if (!afm_config_.enabled || afm_session_ || active_call_ ||
            dmr_candidate_active_locked(now_ms) || !queue_.empty() ||
            !afm_ctcss_qualified_ || afm_rearm_required_ ||
            afm_ambe_queue_.size() < kAmbeFramesPerDmrBurst ||
            afm_ctcss_qualified_since_ms_ <= 0 || dmr_idle_since_ms_ <= 0) {
            return false;
        }
        const std::int64_t guard_ms = afm_config_.dmr_idle_guard_ms;
        if (now_ms - afm_ctcss_qualified_since_ms_ < guard_ms ||
            now_ms - dmr_idle_since_ms_ < guard_ms) {
            return false;
        }

        TxRequest request;
        request.origin = TxOrigin::AnalogFm;
        request.profile = config_.dmr.profile;
        request.slot = afm_config_.dmr_tx.slot;
        request.color_code = afm_config_.dmr_tx.color_code;
        request.source_id = afm_config_.dmr_tx.source_id;
        request.destination_id = afm_config_.dmr_tx.destination_id;
        request.call_type = afm_config_.dmr_tx.call_type;
        request.correlation_id = "afm-" +
            std::to_string(++afm_correlation_sequence_);
        const RouteDecision decision = router_.route_analog_fm(
            request, true, true);
        if (!decision.accepted || !decision.tx_request) {
            afm_rearm_required_ = true;
            audit_.emit({"AFM", "afm_relay.rejected", "route_call", "rejected",
                         request.correlation_id,
                         {{"reason", to_string(decision.reason)}}});
            return false;
        }
        const AutomaticTransmitGrant grant = gate_.evaluate(
            *decision.tx_request, {}, now_ms);
        if (!grant.granted) {
            afm_rearm_required_ = true;
            audit_.emit({"AFM", "afm_relay.rejected", "safe_gate", "rejected",
                         request.correlation_id,
                         {{"reason", to_string(grant.reason)}}});
            return false;
        }

        ActiveCall call;
        call.source_id = afm_config_.dmr_tx.source_id;
        call.destination_id = afm_config_.dmr_tx.destination_id;
        call.call_type = afm_config_.dmr_tx.call_type;
        call.slot = afm_config_.dmr_tx.slot;
        call.color_code = afm_config_.dmr_tx.color_code;
        call.started_at_ms = now_ms;
        call.last_valid_burst_at_ms = now_ms;
        call.relay_started_at_ms = now_ms;
        call.expires_at_ms = grant.expires_at_ms;
        call.correlation_id = request.correlation_id;
        afm_session_ = AfmSession{call, AfmStage::Header, 0, 0, 0, {}};
        starts.push_back(call);
        ++afm_sessions_;
        return true;
    }

    void finish_afm_locked(std::int64_t now_ms,
                           const std::string& reason,
                           std::vector<EndedCall>& ends)
    {
        if (!afm_session_) {
            return;
        }
        const ActiveCall call = afm_session_->call;
        const std::int64_t duration_ms = std::max<std::int64_t>(
            0, now_ms - call.relay_started_at_ms);
        ends.push_back({call, reason, duration_ms});
        afm_session_.reset();
        afm_ambe_queue_.clear();
    }

    bool produce_afm_locked(std::uint8_t* output,
                            float* frame_gate,
                            std::int64_t now_ms,
                            std::vector<ActiveCall>& starts,
                            std::vector<EndedCall>& ends)
    {
        if (!afm_session_ && !start_afm_locked(now_ms, starts)) {
            return false;
        }

        AfmSession& session = *afm_session_;
        if (session.stage != AfmStage::Terminator) {
            if (!afm_ctcss_qualified_) {
                session.stage = AfmStage::Terminator;
                session.stage_count = 0;
                session.end_reason = "ctcss_lost";
                afm_ambe_queue_.clear();
            } else if (now_ms >= session.call.expires_at_ms) {
                session.stage = AfmStage::Terminator;
                session.stage_count = 0;
                session.end_reason = "duration_limit";
                afm_rearm_required_ = true;
                afm_ambe_queue_.clear();
            }
        }

        dmr_b210::DirectFrameBuilder::Burst burst{};
        if (session.stage == AfmStage::Header) {
            burst = afm_builder_.header_burst();
            if (++session.stage_count >= kAfmHeaderBursts) {
                session.stage = AfmStage::Voice;
                session.stage_count = 0;
            }
        } else if (session.stage == AfmStage::Voice) {
            if (afm_ambe_queue_.size() < kAmbeFramesPerDmrBurst) {
                if (++session.underflow_frames >= 5U) {
                    session.stage = AfmStage::Terminator;
                    session.stage_count = 0;
                    session.end_reason = "pcm_underflow";
                    afm_rearm_required_ = true;
                }
                return false;
            }
            std::array<std::uint8_t, 108> ambe{};
            for (std::size_t item = 0;
                 item < kAmbeFramesPerDmrBurst; ++item) {
                std::copy(afm_ambe_queue_.front().begin(),
                          afm_ambe_queue_.front().end(),
                          ambe.begin() + item *
                              dmr_b210::DirectFrameBuilder::kAmbeDibits);
                afm_ambe_queue_.pop_front();
            }
            session.underflow_frames = 0;
            burst = afm_builder_.voice_burst(ambe.data(), session.voice_index++);
            ++afm_voice_bursts_;
        } else {
            burst = afm_builder_.terminator_burst();
            if (++session.stage_count >= kAfmTerminatorBursts) {
                const std::string reason = session.end_reason.empty()
                    ? "ctcss_lost"
                    : session.end_reason;
                afm_builder_.emit_frame(burst, output, frame_gate);
                ++transmitted_bursts_;
                finish_afm_locked(now_ms, reason, ends);
                return true;
            }
        }

        afm_builder_.emit_frame(burst, output, frame_gate);
        ++transmitted_bursts_;
        return true;
    }

    void print_call_event(const char* label,
                          const ActiveCall& call,
                          const char* reason = nullptr,
                          std::optional<std::int64_t> duration_ms = std::nullopt)
    {
        CallConsoleEvent event;
        event.label = label;
        event.source_id = call.source_id;
        event.destination_id = call.destination_id;
        event.call_type = call.call_type;
        event.color_code = call.color_code;
        event.slot = call.slot;
        event.correlation_id = call.correlation_id;
        if (reason) {
            event.reason = reason;
        }
        event.duration_ms = duration_ms;
        std::lock_guard<std::mutex> lock(console_mutex_);
        write_console_message(std::cout, format_call_console_bodies(event));
    }

    static bool is_voice_frame(const std::string& type)
    {
        return type.size() == 7 && type.compare(0, 6, "VOICE_") == 0 &&
            type[6] >= 'A' && type[6] <= 'F';
    }

    bool dmr_candidate_active_locked(std::int64_t now_ms) const
    {
        return dmr_candidate_until_ms_ > now_ms;
    }

    void purge_pre_admission_locked(std::int64_t now_ms)
    {
        while (!pre_admission_bursts_.empty() &&
               now_ms - pre_admission_bursts_.front().received_at_ms >
                   kMaximumRelayStartLatencyMs) {
            pre_admission_bursts_.pop_front();
        }
    }

    void refresh_dmr_idle_locked(std::int64_t now_ms)
    {
        if (dmr_candidate_until_ms_ > 0 &&
            now_ms >= dmr_candidate_until_ms_) {
            dmr_candidate_until_ms_ = 0;
            purge_pre_admission_locked(now_ms);
            if (!active_call_) {
                dmr_idle_since_ms_ = now_ms;
            }
        }
    }

    void update_recording_voice_state_locked(
        const DmrBurstSyncObservation& sync)
    {
        if (!sync.valid) {
            return;
        }
        if (sync.kind == DmrBurstSyncKind::Voice) {
            dmr_recording_voice_active_ = true;
        } else if (sync.kind == DmrBurstSyncKind::Data ||
                   sync.kind == DmrBurstSyncKind::Reverse) {
            dmr_recording_voice_active_ = false;
        }
    }

    void handle_raw_burst(const pmt::pmt_t& message)
    {
        if (!pmt::is_u8vector(message)) {
            return;
        }
        const std::vector<std::uint8_t> values =
            pmt::u8vector_elements(message);
        if (values.size() != kDmrBurstDibits) {
            return;
        }

        RawDmrBurst raw{};
        std::copy(values.begin(), values.end(), raw.begin());
        const DmrBurstSyncObservation sync = inspect_dmr_burst_sync(raw);
        if (sync.valid) {
            dmr_admission_tracker_.mark_sync();
        }
        const std::int64_t now_ms = monotonic_ms();
        bool candidate_started = false;
        bool release_afm_activity = false;
        std::string recording_correlation;
        bool record_burst = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_dmr_idle_locked(now_ms);
            purge_pre_admission_locked(now_ms);

            if (sync.valid) {
                candidate_started = !active_call_ &&
                    !dmr_candidate_active_locked(now_ms);
                dmr_candidate_until_ms_ = now_ms +
                    kMaximumRelayStartLatencyMs;
                dmr_idle_since_ms_ = 0;
                afm_rearm_required_ = true;
                if (afm_ctcss_qualified_) {
                    afm_ctcss_qualified_ = false;
                    afm_ctcss_qualified_since_ms_ = 0;
                    release_afm_activity = true;
                }
                if (afm_session_ &&
                    afm_session_->stage != AfmStage::Terminator) {
                    afm_session_->stage = AfmStage::Terminator;
                    afm_session_->stage_count = 0;
                    afm_session_->end_reason = "dmr_detected";
                    afm_ambe_queue_.clear();
                }
            }

            last_raw_burst_ = raw;
            last_raw_burst_at_ms_ = now_ms;
            if (active_call_) {
                const bool is_fallback_duplicate =
                    fallback_raw_burst_ &&
                    now_ms <= fallback_raw_burst_until_ms_ &&
                    *fallback_raw_burst_ == raw;
                if (is_fallback_duplicate) {
                    fallback_raw_burst_.reset();
                } else if (enqueue_locked(raw, active_call_->slot)) {
                    active_call_->last_valid_burst_at_ms = now_ms;
                    update_recording_voice_state_locked(sync);
                    record_burst = dmr_recording_voice_active_;
                    recording_correlation = active_call_->correlation_id;
                }
            } else if (sync.valid || dmr_candidate_active_locked(now_ms)) {
                if (pre_admission_bursts_.size() >=
                    kMaximumPreAdmissionBursts) {
                    pre_admission_bursts_.pop_front();
                }
                pre_admission_bursts_.push_back({raw, sync, now_ms});
            }
        }

        if (release_afm_activity && afm_rx_activity_) {
            afm_rx_activity_(false);
        }
        if (candidate_started) {
            audit_.emit({"RPT", "signal.classified", "classify_rf", "dmr",
                         "",
                         {{"mode", "DMR"},
                          {"sync_kind", sync_kind_name(sync.kind)},
                          {"sync_bit_errors", std::to_string(sync.bit_errors)},
                          {"direct_slot", std::to_string(sync.direct_slot)}}});
        }
        if (record_burst) {
            const std::optional<double> rssi_dbfs = dmr_receive_metrics_
                ? dmr_receive_metrics_->snapshot().signal_dbfs
                : std::nullopt;
            recording_->submit_dmr_burst(recording_correlation, raw,
                                         rssi_dbfs);
        }
    }


    void handle_frame(const pmt::pmt_t& message)
    {
        if (!pmt::is_dict(message)) {
            reject("invalid_integrity", "PMT", "FRAME");
            return;
        }
        const std::string type = dict_symbol(message, "data_type_str");
        const std::vector<std::uint8_t> raw = dict_u8vector(message, "raw_dibits");
        if (raw.size() != kBurstDibits) {
            reject("invalid_integrity", type.empty() ? "UNKNOWN" : type,
                   "FRAME");
            return;
        }

        if (type == "VOICE_LC_HEADER") {
            handle_header(message, raw);
            return;
        }
        if (is_voice_frame(type)) {
            return;
        }
        if (type == "TERMINATOR_LC") {
            handle_terminator(message, raw);
            return;
        }
        // Data sync still suppresses the FM fallback, but unsupported data
        // must not leak into a later voice admission queue.
        if (type == "CSBK" || type == "DATA_HEADER" ||
            type == "RATE_1_2_DATA") {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pre_admission_bursts_.clear();
            }
            reject("unsupported_profile", type, "DATA");
            return;
        }
        reject("unsupported_profile", type.empty() ? "UNKNOWN" : type,
               "DATA");
    }


    void handle_header(const pmt::pmt_t& message,
                       const std::vector<std::uint8_t>& raw)
    {
        const auto source = dict_long(message, "source_id");
        const auto destination = dict_long(message, "dest_id");
        const auto metadata_call_type = dict_long(message, "call_type");
        const auto slot = dict_long(message, "slot");
        const auto color_code = dict_long(message, "color_code");
        if (!dict_bool(message, "sync_valid")) {
            reject("invalid_integrity", "VOICE_LC_HEADER", "SYNC");
            return;
        }
        if (!dict_bool(message, "slot_type_valid") || !slot || !color_code ||
            (*slot != 1 && *slot != 2) ||
            *color_code < 0 || *color_code > 15) {
            reject("invalid_integrity", "VOICE_LC_HEADER", "SLOT");
            return;
        }
        if (!dict_bool(message, "lc_valid") || !source || !destination ||
            !metadata_call_type || *source <= 0 || *destination <= 0) {
            reject("invalid_integrity", "VOICE_LC_HEADER", "LC");
            return;
        }

        DmrEvent event;
        event.profile = config_.dmr.profile;
        event.kind = DmrEventKind::Voice;
        event.integrity = DmrIntegrity::Valid;
        event.source_id = static_cast<std::uint32_t>(*source);
        event.destination_id = static_cast<std::uint32_t>(*destination);
        event.call_type = call_type_from_metadata(
            *metadata_call_type, *event.destination_id);
        event.slot = static_cast<int>(*slot);
        event.color_code = static_cast<int>(*color_code);
        event.correlation_id = "pending";

        report_receive_call(RelaySource::Dmr, true, *event.source_id,
                            *event.destination_id, event.call_type);

        if (forwarding_enabled_ &&
            !forwarding_enabled_->load(std::memory_order_relaxed)) {
            reject("tx_disabled", "VOICE_LC_HEADER", "CTRL");
            return;
        }

        const RouteDecision decision = router_.route_dmr_event(event);
        if (!decision.accepted || !decision.tx_request) {
            reject(to_string(decision.reason), "VOICE_LC_HEADER", "ROUTE");
            return;
        }
        const std::int64_t now_ms = monotonic_ms();
        const AutomaticTransmitGrant grant = gate_.evaluate(
            *decision.tx_request, {}, now_ms);
        if (!grant.granted) {
            reject(to_string(grant.reason), "VOICE_LC_HEADER", "SAFE");
            return;
        }

        bool begin_activity = false;
        bool state_conflict = false;
        bool queue_failed = false;
        std::optional<ActiveCall> started_call;
        std::vector<RawDmrBurst> recording_bursts;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_dmr_idle_locked(now_ms);
            purge_pre_admission_locked(now_ms);
            enforce_duration_limit_locked(now_ms);
            enforce_inactivity_timeout_locked(now_ms);
            if (active_call_ &&
                (active_call_->source_id != *event.source_id ||
                 active_call_->destination_id != *event.destination_id ||
                 active_call_->call_type != event.call_type ||
                 active_call_->slot != event.slot ||
                 active_call_->color_code != event.color_code)) {
                state_conflict = true;
            } else {
                begin_activity = !active_call_.has_value();
                if (begin_activity) {
                    event.correlation_id =
                        "rf-" + std::to_string(++correlation_sequence_);
                    const std::int64_t detected_at_ms =
                        pre_admission_bursts_.empty()
                            ? now_ms
                            : pre_admission_bursts_.front().received_at_ms;
                    active_call_ = ActiveCall{
                        *event.source_id, *event.destination_id, event.call_type,
                        event.slot, event.color_code, detected_at_ms, now_ms, 0,
                        grant.expires_at_ms, event.correlation_id};
                    dmr_idle_since_ms_ = 0;
                    afm_ctcss_qualified_ = false;
                    afm_ctcss_qualified_since_ms_ = 0;
                    afm_rearm_required_ = true;
                    dmr_recording_voice_active_ = false;
                } else {
                    event.correlation_id = active_call_->correlation_id;
                }

                if (begin_activity) {
                    const std::size_t queue_size_before = queue_.size();
                    bool queued = true;
                    if (!pre_admission_bursts_.empty()) {
                        bool first = true;
                        for (const PreAdmissionBurst& pending :
                             pre_admission_bursts_) {
                            if (!enqueue_locked(
                                    pending.dibits, event.slot,
                                    first ? &*active_call_ : nullptr)) {
                                queued = false;
                                break;
                            }
                            first = false;
                            update_recording_voice_state_locked(pending.sync);
                            if (dmr_recording_voice_active_) {
                                recording_bursts.push_back(pending.dibits);
                            }
                        }
                    } else {
                        queued = enqueue_locked(raw, event.slot, &*active_call_);
                        if (queued) {
                            RawDmrBurst fallback{};
                            std::copy(raw.begin(), raw.end(), fallback.begin());
                            fallback_raw_burst_ = fallback;
                            fallback_raw_burst_until_ms_ = now_ms + 200;
                        }
                    }
                    pre_admission_bursts_.clear();
                    if (!queued) {
                        queue_.resize(queue_size_before);
                        active_call_.reset();
                        dmr_idle_since_ms_ = now_ms;
                        dmr_recording_voice_active_ = false;
                        queue_failed = true;
                    }
                }
                if (!queue_failed) {
                    active_call_->last_valid_burst_at_ms = now_ms;
                    if (begin_activity) {
                        started_call = active_call_;
                    }
                }
            }
        }
        if (state_conflict) {
            reject("resource_busy", "VOICE_LC_HEADER", "STATE");
            return;
        }
        if (queue_failed) {
            reject("resource_busy", "VOICE_LC_HEADER", "QUEUE", false);
            return;
        }
        dmr_admission_tracker_.mark_admitted();
        if (begin_activity) {
            if (dmr_rx_activity_) {
                dmr_rx_activity_(true);
            }
            if (afm_rx_activity_) {
                afm_rx_activity_(false);
            }
            report_receive_call(RelaySource::Fm, false);
        }
        if (begin_activity) {
            recording_->start_call(recording_metadata(
                *started_call, RecordingMode::DmrRelay));
            publish_relay_event(
                "detected", RelaySource::Dmr, *started_call, 0);
            const std::optional<double> rssi_dbfs = dmr_receive_metrics_
                ? dmr_receive_metrics_->snapshot().signal_dbfs
                : std::nullopt;
            for (const RawDmrBurst& burst : recording_bursts) {
                recording_->submit_dmr_burst(
                    started_call->correlation_id, burst, rssi_dbfs);
            }
            print_call_event("CALL START", *started_call);
            {
                std::lock_guard<std::mutex> console_lock(console_mutex_);
                write_console_message(
                    std::cout,
                    format_receive_signal_status(
                        "DMR", dmr_receive_metrics_));
            }
            audit_.emit({"RPT", "dmr_relay.admitted", "forward_call", "accepted",
                         event.correlation_id,
                         {{"source_id", std::to_string(*event.source_id)},
                          {"destination_id", std::to_string(*event.destination_id)},
                          {"call_type", to_string(event.call_type)},
                          {"slot", std::to_string(event.slot)},
                          {"color_code", std::to_string(event.color_code)},
                          {"detected_at_monotonic_ms", std::to_string(
                               started_call->started_at_ms)}}});
        }
    }

    void handle_terminator(const pmt::pmt_t& message,
                           const std::vector<std::uint8_t>& raw)
    {
        const auto source = dict_long(message, "source_id");
        const auto destination = dict_long(message, "dest_id");
        const auto slot = dict_long(message, "slot");
        const auto color_code = dict_long(message, "color_code");
        if (!dict_bool(message, "sync_valid") ||
            !dict_bool(message, "slot_type_valid") ||
            !dict_bool(message, "lc_valid") || !source || !destination ||
            !slot || !color_code) {
            reject("invalid_integrity", "TERMINATOR_LC", "LC");
            return;
        }
        std::optional<ActiveCall> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::int64_t now_ms = monotonic_ms();
            if (!active_call_) {
                ++rejected_frames_;
                return;
            }
            if (active_call_->source_id != static_cast<std::uint32_t>(*source) ||
                active_call_->destination_id != static_cast<std::uint32_t>(*destination) ||
                active_call_->slot != *slot ||
                active_call_->color_code != *color_code) {
                ++rejected_frames_;
                return;
            }
            RawDmrBurst terminator{};
            std::copy(raw.begin(), raw.end(), terminator.begin());
            const bool raw_already_received = last_raw_burst_ &&
                now_ms - last_raw_burst_at_ms_ <= 200 &&
                *last_raw_burst_ == terminator;
            if (!raw_already_received &&
                !enqueue_locked(terminator, active_call_->slot)) {
                return;
            }
            finished = active_call_;
            active_call_.reset();
            dmr_recording_voice_active_ = false;
            fallback_raw_burst_.reset();
            dmr_idle_since_ms_ = now_ms;
        }
        if (dmr_rx_activity_) {
            dmr_rx_activity_(false);
        }
        report_receive_call(RelaySource::Dmr, false);
        recording_->finish_call(finished->correlation_id, "terminator");
        const std::int64_t now_ms = monotonic_ms();
        const std::int64_t relay_duration_ms =
            actual_relay_duration_ms(*finished, now_ms);
        audit_.emit({"RPT", "dmr_relay.ended", "forward_call", "ok",
                     finished->correlation_id,
                     {{"source_id", std::to_string(finished->source_id)},
                      {"destination_id", std::to_string(finished->destination_id)},
                      {"reason", "terminator"},
                      {"relay_duration_ms", std::to_string(relay_duration_ms)},
                      {"relay_duration_hms", format_duration_hms(relay_duration_ms)}}});
        publish_relay_event(
            "ended", RelaySource::Dmr, *finished, relay_duration_ms);
        const std::int64_t duration_ms =
            std::max<std::int64_t>(0, now_ms - finished->started_at_ms);
        print_call_event("CALL END", *finished, "terminator", duration_ms);
    }

    bool enqueue_locked(const std::vector<std::uint8_t>& raw,
                        int slot,
                        const ActiveCall* call_start = nullptr)
    {
        if (raw.size() != kDmrBurstDibits) {
            return false;
        }
        RawDmrBurst burst{};
        std::copy(raw.begin(), raw.end(), burst.begin());
        return enqueue_locked(burst, slot, call_start);
    }

    bool enqueue_locked(const RawDmrBurst& raw,
                        int slot,
                        const ActiveCall* call_start = nullptr)
    {
        if (queue_.size() >= kMaximumQueuedBursts) {
            ++rejected_frames_;
            audit_.emit({"RPT", "tx_queue.overflow", "enqueue_burst", "failed",
                         active_call_ ? active_call_->correlation_id : std::string{},
                         {{"queue_limit", std::to_string(kMaximumQueuedBursts)}}});
            return false;
        }
        QueuedBurst burst;
        burst.dibits = raw;
        burst.slot = slot;
        if (call_start) {
            burst.call_start = *call_start;
        }
        queue_.push_back(std::move(burst));
        ++accepted_bursts_;
        return true;
    }

    static std::int64_t actual_relay_duration_ms(const ActiveCall& call,
                                                 std::int64_t now_ms)
    {
        if (call.relay_started_at_ms <= 0 || now_ms < call.relay_started_at_ms) {
            return 0;
        }
        return now_ms - call.relay_started_at_ms;
    }

    void enforce_duration_limit_locked(std::int64_t now_ms)
    {
        if (!active_call_ || now_ms < active_call_->expires_at_ms) {
            return;
        }
        const ActiveCall limited = *active_call_;
        cooldowns_.record_duration_limit(
            limited.source_id, now_ms,
            config_.transmit.source_cooldown_seconds);
        audit_.emit({"SAFE", "transmit.duration_limit", "stop_call", "limited",
                     limited.correlation_id,
                     {{"source_id", std::to_string(limited.source_id)}}});
        const std::int64_t relay_duration_ms =
            actual_relay_duration_ms(limited, now_ms);
        audit_.emit({"RPT", "dmr_relay.ended", "forward_call", "limited",
                     limited.correlation_id,
                     {{"source_id", std::to_string(limited.source_id)},
                      {"destination_id", std::to_string(limited.destination_id)},
                      {"reason", "duration_limit"},
                      {"relay_duration_ms", std::to_string(relay_duration_ms)},
                      {"relay_duration_hms", format_duration_hms(relay_duration_ms)}}});
        publish_relay_event(
            "ended", RelaySource::Dmr, limited, relay_duration_ms);
        active_call_.reset();
        queue_.clear();
        fallback_raw_burst_.reset();
        dmr_recording_voice_active_ = false;
        dmr_idle_since_ms_ = now_ms;
        ++rejected_frames_;
        recording_->finish_call(limited.correlation_id, "duration_limit");
        if (dmr_rx_activity_) {
            dmr_rx_activity_(false);
        }
        report_receive_call(RelaySource::Dmr, false);
        const std::int64_t duration_ms =
            std::max<std::int64_t>(0, now_ms - limited.started_at_ms);
        print_call_event("CALL END", limited, "duration_limit", duration_ms);
    }

    void enforce_inactivity_timeout_locked(std::int64_t now_ms)
    {
        if (!active_call_ ||
            !call_inactivity_expired(active_call_->last_valid_burst_at_ms,
                                     now_ms,
                                     config_.dmr.receive_inactivity_timeout_ms)) {
            return;
        }
        const ActiveCall finished = *active_call_;
        active_call_.reset();
        fallback_raw_burst_.reset();
        dmr_recording_voice_active_ = false;
        dmr_idle_since_ms_ = now_ms;
        recording_->finish_call(
            finished.correlation_id, "inactivity_timeout");
        if (dmr_rx_activity_) {
            dmr_rx_activity_(false);
        }
        report_receive_call(RelaySource::Dmr, false);
        const std::int64_t relay_duration_ms =
            actual_relay_duration_ms(finished, now_ms);
        audit_.emit({"RPT", "dmr_relay.ended", "forward_call", "ok",
                     finished.correlation_id,
                     {{"source_id", std::to_string(finished.source_id)},
                      {"destination_id", std::to_string(finished.destination_id)},
                      {"reason", "inactivity_timeout"},
                      {"relay_duration_ms", std::to_string(relay_duration_ms)},
                      {"relay_duration_hms", format_duration_hms(relay_duration_ms)}}});
        publish_relay_event(
            "ended", RelaySource::Dmr, finished, relay_duration_ms);
        const std::int64_t duration_ms =
            std::max<std::int64_t>(0, now_ms - finished.started_at_ms);
        print_call_event(
            "CALL END", finished, "inactivity_timeout", duration_ms);
    }


    void emit_rejection(const std::string& reason,
                        const std::string& type,
                        const std::string& stage,
                        bool show_console,
                        bool increment_count)
    {
        if (increment_count) {
            ++rejected_frames_;
        }
        std::map<std::string, std::string> fields{
            {"failure_stage", stage},
            {"reason", reason},
            {"frame_type", type},
        };
        if (dmr_receive_metrics_) {
            fields["activity_threshold_dbfs"] = format_decimal(
                dmr_receive_metrics_->activity_threshold_dbfs());
            const ReceiveSignalSnapshot snapshot =
                dmr_receive_metrics_->snapshot();
            if (snapshot.signal_dbfs) {
                fields["signal_dbfs"] = format_decimal(*snapshot.signal_dbfs);
            }
            if (snapshot.noise_dbfs) {
                fields["noise_dbfs"] = format_decimal(*snapshot.noise_dbfs);
            }
            if (snapshot.snr_db) {
                fields["snr_db"] = format_decimal(*snapshot.snr_db);
            }
        }
        audit_.emit({"RPT", "dmr_relay.rejected", "route_frame", "rejected", "",
                     std::move(fields)});
        if (show_console) {
            std::lock_guard<std::mutex> console_lock(console_mutex_);
            write_console_message(
                std::cout,
                format_signal_reject_console_bodies({stage, reason}));
        }
    }

    void reject(const std::string& reason,
                const std::string& type,
                const std::string& stage,
                bool increment_count = true)
    {
        const bool show_console = dmr_admission_tracker_.mark_failure();
        emit_rejection(reason, type, stage, show_console, increment_count);
    }

    RepeaterConfig config_;
    OperationAuditLogger& audit_;
    std::shared_ptr<AudioRecordingRuntime> recording_;
    std::shared_ptr<ReceiveSignalMetrics> dmr_receive_metrics_;
    std::shared_ptr<ReceiveSignalMetrics> fm_receive_metrics_;
    DmrAdmissionTracker dmr_admission_tracker_;
    DmrSourceCooldownStore cooldowns_;
    CallRouter router_;
    AutomaticTransmitGate gate_;
    AnalogFmFallbackConfig afm_config_;
    int startup_prefill_frames_remaining_ = 0;
    int tx_pipeline_lead_ms_ = 0;
    dmr_b210::DirectFrameBuilder afm_builder_;
    ActivityCallback dmr_rx_activity_;
    ActivityCallback afm_rx_activity_;
    ActivityCallback tx_activity_;
    RelayEventCallback relay_event_;
    ReceiveCallCallback receive_call_;
    std::shared_ptr<std::atomic_bool> forwarding_enabled_;
    bool rx_diagnostic_ = false;
    mutable std::mutex mutex_;
    std::mutex console_mutex_;
    std::deque<QueuedBurst> queue_;
    std::deque<PreAdmissionBurst> pre_admission_bursts_;
    std::deque<std::array<std::uint8_t,
                          dmr_b210::DirectFrameBuilder::kAmbeDibits>>
        afm_ambe_queue_;
    std::optional<ActiveCall> active_call_;
    std::optional<AfmSession> afm_session_;
    std::optional<RawDmrBurst> last_raw_burst_;
    std::optional<RawDmrBurst> fallback_raw_burst_;
    bool tx_asserted_ = false;
    bool afm_ctcss_qualified_ = false;
    bool afm_rearm_required_ = false;
    double afm_ctcss_confidence_db_ = 0.0;
    bool dmr_recording_voice_active_ = false;
    std::int64_t afm_ctcss_qualified_since_ms_ = 0;
    std::int64_t next_afm_ctcss_status_at_ms_ = 0;
    std::int64_t dmr_idle_since_ms_ = 0;
    std::int64_t dmr_candidate_until_ms_ = 0;
    std::int64_t last_raw_burst_at_ms_ = 0;
    std::int64_t fallback_raw_burst_until_ms_ = 0;
    std::uint64_t correlation_sequence_ = 0;
    std::uint64_t afm_correlation_sequence_ = 0;
    std::atomic<std::uint64_t> accepted_bursts_{0};
    std::atomic<std::uint64_t> transmitted_bursts_{0};
    std::atomic<std::uint64_t> rejected_frames_{0};
    std::atomic<std::uint64_t> afm_sessions_{0};
    std::atomic<std::uint64_t> afm_voice_bursts_{0};
    std::atomic<std::uint64_t> afm_dropped_ambe_frames_{0};
};

class HardwareB210Session final : public B210Session {
public:
    HardwareB210Session(ValidatedConfig config, OperationAuditLogger& audit,
                        bool rx_diagnostic,
                        std::shared_ptr<NetworkEventSink> network,
                        std::shared_ptr<std::atomic_bool> forwarding_enabled,
                        std::shared_ptr<RxSignalCalibrationRuntime> calibration,
                        std::function<void()> recording_storage_update)
        : config_(std::move(config)), audit_(audit),
          network_(std::move(network)),
          forwarding_enabled_(std::move(forwarding_enabled)),
          calibration_(std::move(calibration)),
          recording_storage_update_(std::move(recording_storage_update)),
          rx_diagnostic_(rx_diagnostic)
    {
    }

    ~HardwareB210Session() override
    {
        stop();
    }

    void start(const ValidatedRfConfig& rf) override
    {
        if (running_) {
            throw std::runtime_error("B210 hardware session already running");
        }
        config_.config.radio = rf.radio;
        config_.config.io_status = rf.io_status;
        const auto profile = std::find_if(
            config_.config.channel_profiles.begin(),
            config_.config.channel_profiles.end(),
            [&](const ChannelProfile& item) {
                return item.id == rf.active_profile.id;
            });
        if (profile == config_.config.channel_profiles.end()) {
            config_.config.channel_profiles.push_back(rf.active_profile);
        } else {
            *profile = rf.active_profile;
        }
        config_.config.radio.active_channel_profile_id =
            rf.active_channel_profile_id;
        if (config_.config.dmr.profile != DmrProfile::DirectLab) {
            throw std::runtime_error("MissingVectorSet: hardware runtime only enables direct_lab");
        }
        if (rf.radio.sample_rate_hz != static_cast<std::int64_t>(kUsrpRate)) {
            throw std::runtime_error("hardware runtime requires 480000 sample/s");
        }

        const RfEndpointConfig& rx = rf.active_profile.dmr_rx;
        const RfEndpointConfig& tx = rf.active_profile.dmr_tx;
        const AnalogFmFallbackConfig& afm =
            rf.active_profile.analog_fm_fallback;
        const bool afm_enabled = afm.enabled;
        const double rx_hardware_center =
            static_cast<double>(rx.frequency_hz + rx.lo_offset_hz);
        const double tx_hardware_center =
            static_cast<double>(tx.frequency_hz + tx.lo_offset_hz);
        const double rx_gain_db = rx.gain_tenths_db / 10.0;
        const double tx_gain_db = tx.gain_tenths_db / 10.0;
        double applied_rx_center = 0.0;
        double applied_tx_center = 0.0;
        double applied_rx_gain = 0.0;
        double applied_afm_rx_center = 0.0;
        double applied_afm_rx_gain = 0.0;
        double applied_tx_gain = 0.0;
        const auto uhd_step = [&](const char* name, auto&& action) {
            try {
                action();
            } catch (const std::exception& error) {
                throw std::runtime_error(
                    std::string(name) + " (RX channel " +
                    std::to_string(rx.channel) + ", TX channel " +
                    std::to_string(tx.channel) + "): " + error.what());
            }
        };
        constexpr std::size_t rx_stream_channel = 0;
        constexpr std::size_t afm_stream_channel = 1;
        ::uhd::stream_args_t rx_args("fc32");
        rx_args.channels = {static_cast<std::size_t>(rx.channel)};
        if (afm_enabled) {
            rx_args.channels.push_back(
                static_cast<std::size_t>(afm.rx.channel));
        }
        uhd_step("create RX source", [&] {
            source_ = gr::uhd::usrp_source::make(
                ::uhd::device_addr_t(rf.radio.uhd_device), rx_args);
        });
        uhd_step("set RX sample rate", [&] {
            source_->set_samp_rate(kUsrpRate);
            require_precision(kUsrpRate, source_->get_samp_rate(), 0.5,
                              "RX sample rate");
        });
        uhd_step("validate RX capability", [&] {
            require_in_range(source_->get_freq_range(rx_stream_channel),
                             rx_hardware_center, 0.5, "RX frequency");
            require_in_range(source_->get_gain_range(rx_stream_channel),
                             rx_gain_db, 0.05, "RX gain");
        });
        uhd_step("set RX center frequency", [&] {
            source_->set_center_freq(rx_hardware_center, rx_stream_channel);
            applied_rx_center = source_->get_center_freq(rx_stream_channel);
            require_precision(rx_hardware_center, applied_rx_center, 0.5,
                              "RX frequency");
        });
        uhd_step("set RX gain", [&] {
            source_->set_gain(rx_gain_db, rx_stream_channel);
            applied_rx_gain = source_->get_gain(rx_stream_channel);
            require_precision(rx_gain_db, applied_rx_gain, 0.05, "RX gain");
        });
        {
            std::lock_guard<std::mutex> lock(rx_config_mutex_);
            rx_stream_channels_[rx.channel] = rx_stream_channel;
            rx_gain_tenths_db_[rx.channel] = rx.gain_tenths_db;
        }
        uhd_step("set RX antenna", [&] {
            source_->set_antenna(rx.antenna, rx_stream_channel);
        });
        uhd_step("set RX bandwidth", [&] {
            source_->set_bandwidth(static_cast<double>(rx.bandwidth_hz),
                                   rx_stream_channel);
        });
        if (afm_enabled) {
            uhd_step("validate analog FM RX capability", [&] {
                require_in_range(source_->get_freq_range(afm_stream_channel),
                                 rx_hardware_center, 0.5,
                                 "analog FM RX frequency");
                require_in_range(source_->get_gain_range(afm_stream_channel),
                                 afm.rx.gain_tenths_db / 10.0, 0.05,
                                 "analog FM RX gain");
            });
            uhd_step("set analog FM RX center frequency", [&] {
                source_->set_center_freq(rx_hardware_center, afm_stream_channel);
                applied_afm_rx_center =
                    source_->get_center_freq(afm_stream_channel);
                require_precision(rx_hardware_center, applied_afm_rx_center, 0.5,
                                  "analog FM RX frequency");
            });
            uhd_step("set analog FM RX gain", [&] {
                source_->set_gain(afm.rx.gain_tenths_db / 10.0,
                                  afm_stream_channel);
                applied_afm_rx_gain = source_->get_gain(afm_stream_channel);
                require_precision(afm.rx.gain_tenths_db / 10.0,
                                  applied_afm_rx_gain, 0.05,
                                  "analog FM RX gain");
            });
            {
                std::lock_guard<std::mutex> lock(rx_config_mutex_);
                rx_stream_channels_[afm.rx.channel] = afm_stream_channel;
                rx_gain_tenths_db_[afm.rx.channel] = afm.rx.gain_tenths_db;
            }
            uhd_step("set analog FM RX antenna", [&] {
                source_->set_antenna(afm.rx.antenna, afm_stream_channel);
            });
            uhd_step("set analog FM RX bandwidth", [&] {
                source_->set_bandwidth(
                    static_cast<double>(std::max<std::int64_t>(
                        200000, afm.rx.bandwidth_hz)),
                    afm_stream_channel);
            });
        }

        gpio_ = std::make_unique<GnuradioB210GpioAdapter>(source_);
        io_ = std::make_unique<RuntimeIo>(rf.io_status, *gpio_);
        io_->initialize();

        recording_ = std::make_shared<AudioRecordingRuntime>(
            config_.config.logging.recording_directory,
            static_cast<std::size_t>(config_.config.logging.max_queue_events),
            config_.config.remote_voice,
            [this](const RecordingNotice& notice) {
                handle_recording_notice(notice);
            });
        const double squelch_dbfs =
            config_.config.dmr.receive_squelch_tenths_dbfs / 10.0;
        const auto dmr_receive_metrics = std::make_shared<ReceiveSignalMetrics>(
            squelch_dbfs, 200);
        const auto fm_receive_metrics = std::make_shared<ReceiveSignalMetrics>(
            afm.fm.squelch_tenths_dbfs / 10.0);

        RepeaterConfig runtime_config = config_.config;
        runtime_config.radio = rf.radio;
        runtime_config.io_status = rf.io_status;
        const auto runtime_profile = std::find_if(
            runtime_config.channel_profiles.begin(),
            runtime_config.channel_profiles.end(),
            [&](const ChannelProfile& item) {
                return item.id == rf.active_profile.id;
            });
        if (runtime_profile == runtime_config.channel_profiles.end()) {
            runtime_config.channel_profiles.push_back(rf.active_profile);
        } else {
            *runtime_profile = rf.active_profile;
        }
        runtime_config.radio.active_channel_profile_id =
            rf.active_channel_profile_id;
        relay_ = DirectRelayBurstSource::make(
            runtime_config, audit_, recording_, dmr_receive_metrics,
            fm_receive_metrics,
            [this, channel = rx.channel](bool active) {
                if (io_) io_->rx(channel, active);
            },
            [this, channel = afm.rx.channel](bool active) {
                if (io_) io_->rx(channel, active);
            },
            [this, channel = tx.channel, mirrored = afm_enabled](bool active) {
                if (!io_) {
                    return;
                }
                if (mirrored) {
                    io_->tx(0, active);
                    io_->tx(1, active);
                    return;
                }
                io_->tx(channel, active);
            }, rx_diagnostic_,
            [this](const NetworkRelayEvent& event) {
                if (network_) {
                    network_->publish_relay_event(event);
                }
            },
            [this, dmr_channel = rx.channel, fm_channel = afm.rx.channel,
             dmr_metrics = dmr_receive_metrics,
             fm_metrics = fm_receive_metrics](RelaySource source,
                                                bool receiving,
                                                std::uint32_t source_id,
                                                std::uint32_t destination_id,
                                                CallType call_type) {
                if (!network_) {
                    return;
                }
                const bool fm = source == RelaySource::Fm;
                const std::shared_ptr<ReceiveSignalMetrics>& metrics =
                    fm ? fm_metrics : dmr_metrics;
                const ReceiveSignalSnapshot snapshot = metrics
                    ? metrics->snapshot()
                    : ReceiveSignalSnapshot{};
                const int32_t physical_channel = fm ? fm_channel : dmr_channel;
                const int32_t gain = current_rx_gain(physical_channel);
                if (calibration_) {
                    calibration_->observe(physical_channel, gain,
                                          snapshot.signal_dbfs,
                                          snapshot.noise_dbfs,
                                          snapshot.snr_db, monotonic_ms(),
                                          receiving);
                }
                const RxCalibrationReading reading = calibration_
                    ? calibration_->reading(physical_channel, gain,
                                            snapshot.signal_dbfs)
                    : RxCalibrationReading{};
                ReceiveStatus status;
                status.rx_channel = physical_channel;
                status.receiver_mode = fm ? "fm" : "dmr";
                status.receiving = receiving;
                status.rssi_dbfs = snapshot.signal_dbfs;
                status.rssi_dbm = reading.rssi_dbm;
                status.snr_db = snapshot.snr_db;
                status.calibration_state = reading.calibrated
                    ? "calibrated" : "uncalibrated";
                status.active_call_state_known = true;
                if (receiving) {
                    status.active_call = NetworkReceiveCall{
                        source_id, destination_id,
                        fm ? "fm" : receive_call_mode(call_type)};
                }
                network_->observe_receive_status(status);
            },
            forwarding_enabled_);

        const auto real_taps = gr::filter::firdes::low_pass(
            1.0, kUsrpRate, 12000.0, 3000.0);
        std::vector<gr_complex> channel_taps;
        channel_taps.reserve(real_taps.size());
        for (float tap : real_taps) {
            channel_taps.emplace_back(tap, 0.0F);
        }
        const auto channel_filter =
            gr::filter::freq_xlating_fir_filter_ccc::make(
                static_cast<int>(kUsrpRate / kDemodRate), channel_taps,
                -static_cast<double>(rx.lo_offset_hz), kUsrpRate);
        const auto squelch = gr::analog::pwr_squelch_cc::make(
            squelch_dbfs, 0.01, 0, false);
        dmr_agc_telemetry_ = std::make_shared<ReceiveAgcTelemetry>();
        const auto dmr_agc = ReceiveAgcBlock::make(
            "dmr_receive_agc", config_.config.radio.receive_agc,
            squelch_dbfs,
            dmr_agc_telemetry_);
        const auto fm_demod = gr::analog::quadrature_demod_cf::make(
            static_cast<float>(kDemodRate / (2.0 * kPi * kDmrDeviation)));
        const auto rx_rrc_taps = gr::filter::firdes::root_raised_cosine(
            1.0, kDemodRate, kSymbolRate, 0.2, 101);
        const auto rx_matched_filter = gr::filter::fir_filter_fff::make(
            1, rx_rrc_taps);
        const auto symbol_sampler =
            dmr_b210::SharedDmrBurstSymbolSampler::make(
                rx_diagnostic_, [](const std::string& message) {
                    write_console_message(std::cout, message);
                });
        const auto frame_decoder = gr::dmr::frame_decoder::make(
            static_cast<float>(kSymbolRate), 0, -1, false);
        frame_decoder->set_debug(rx_diagnostic_);

        gr::filter::freq_xlating_fir_filter_ccc::sptr afm_channel_filter;
        gr::analog::pwr_squelch_cc::sptr afm_squelch;
        ReceiveAgcBlock::sptr afm_agc;
        gr::analog::quadrature_demod_cf::sptr afm_demod;
        gr::filter::fir_filter_fff::sptr ctcss_decimator;
        gr::filter::fir_filter_fff::sptr voice_decimator;
        CtcssMonitorBlock::sptr ctcss_monitor;
        gr::blocks::multiply_const_ff::sptr afm_audio_scale;
        gr::blocks::float_to_short::sptr afm_float_to_short;
        gr::op25_repeater::ambe_encoder_sb::sptr afm_ambe_encoder;
        RecordingPcmSink::sptr afm_recording_sink;
        if (afm_enabled) {
            const auto afm_channel_real_taps = gr::filter::firdes::low_pass(
                1.0, kUsrpRate, 12000.0, 3000.0);
            std::vector<gr_complex> afm_channel_taps;
            afm_channel_taps.reserve(afm_channel_real_taps.size());
            for (float tap : afm_channel_real_taps) {
                afm_channel_taps.emplace_back(tap, 0.0F);
            }
            afm_channel_filter =
                gr::filter::freq_xlating_fir_filter_ccc::make(
                    static_cast<int>(kUsrpRate / kDemodRate),
                    afm_channel_taps,
                    -static_cast<double>(rx.lo_offset_hz), kUsrpRate);
            afm_squelch = gr::analog::pwr_squelch_cc::make(
                afm.fm.squelch_tenths_dbfs / 10.0, 0.01, 0, false);
            fm_agc_telemetry_ = std::make_shared<ReceiveAgcTelemetry>();
            afm_agc = ReceiveAgcBlock::make(
                "fm_receive_agc", config_.config.radio.receive_agc,
                afm.fm.squelch_tenths_dbfs / 10.0,
                fm_agc_telemetry_);
            afm_demod = gr::analog::quadrature_demod_cf::make(
                static_cast<float>(kDemodRate /
                    (2.0 * kPi * afm.fm.max_deviation_hz)));
            const auto ctcss_taps = gr::filter::firdes::low_pass(
                1.0, kDemodRate, 300.0, 100.0);
            ctcss_decimator = gr::filter::fir_filter_fff::make(6, ctcss_taps);
            const auto voice_taps = gr::filter::firdes::band_pass(
                1.0, kDemodRate, 250.0,
                static_cast<double>(afm.fm.audio_bandwidth_hz), 150.0);
            voice_decimator = gr::filter::fir_filter_fff::make(6, voice_taps);
            ctcss_monitor = CtcssMonitorBlock::make(
                afm.ctcss, [relay = relay_](const CtcssState& state) {
                    relay->set_afm_ctcss_state(state);
                });
            afm_audio_scale = gr::blocks::multiply_const_ff::make(30000.0F);
            afm_float_to_short = gr::blocks::float_to_short::make(1, 1.0F);
            afm_ambe_encoder = gr::op25_repeater::ambe_encoder_sb::make(0);
            afm_ambe_encoder->set_gain_adjust(3.0F);
            afm_recording_sink = RecordingPcmSink::make(recording_);
        }

        const std::vector<float> symbol_map{
            1.0F / 3.0F, 1.0F, -1.0F / 3.0F, -1.0F};
        const auto tx_symbols = gr::digital::chunks_to_symbols_bf::make(
            symbol_map, 1);
        const auto tx_rrc_taps = gr::filter::firdes::root_raised_cosine(
            kSamplesPerSymbol, kUsrpRate, kSymbolRate, 0.2, kTxRrcTapCount);
        const auto tx_pulse = gr::filter::interp_fir_filter_fff::make(
            kSamplesPerSymbol, tx_rrc_taps);
        const auto tx_fm = gr::analog::frequency_modulator_fc::make(
            static_cast<float>(2.0 * kPi * kDmrDeviation / kUsrpRate));
        const auto tx_gate_repeat = gr::blocks::repeat::make(
            sizeof(float), kSamplesPerSymbol);
        const auto tx_gate_delay = gr::blocks::delay::make(
            sizeof(float), kTxRrcDelaySamples);
        const auto tx_gate_complex = gr::blocks::float_to_complex::make(1);
        const auto tx_gate = gr::blocks::multiply_cc::make(1);
        const auto tx_mixer = gr::blocks::multiply_cc::make(1);
        const auto tx_oscillator = gr::analog::sig_source_c::make(
            kUsrpRate, gr::analog::GR_COS_WAVE,
            -static_cast<double>(tx.lo_offset_hz), 1.0, 0.0);

        ::uhd::stream_args_t tx_args("fc32");
        // The B210 scheduler requires symmetric 2RX+2TX streaming while the
        // analog FM receiver is enabled. Mirror the active DMR waveform to
        // both TX ports; the single-RX mode keeps its one-port TX stream.
        const bool mirrored_tx_streams = afm_enabled;
        tx_args.channels = mirrored_tx_streams
            ? std::vector<std::size_t>{0U, 1U}
            : std::vector<std::size_t>{static_cast<std::size_t>(tx.channel)};
        const std::size_t active_tx_stream_channel = mirrored_tx_streams
            ? static_cast<std::size_t>(tx.channel)
            : 0U;
        uhd_step("create TX sink", [&] {
            sink_ = gr::uhd::usrp_sink::make(
                ::uhd::device_addr_t(rf.radio.uhd_device), tx_args);
        });
        uhd_step("set TX sample rate", [&] {
            sink_->set_samp_rate(kUsrpRate);
            require_precision(kUsrpRate, sink_->get_samp_rate(), 0.5,
                              "TX sample rate");
        });
        for (std::size_t stream_channel = 0;
             stream_channel < tx_args.channels.size(); ++stream_channel) {
            uhd_step("validate TX capability", [&] {
                require_in_range(sink_->get_freq_range(stream_channel),
                                 tx_hardware_center, 0.5, "TX frequency");
                require_in_range(sink_->get_gain_range(stream_channel),
                                 tx_gain_db, 0.05, "TX gain");
            });
            uhd_step("set TX center frequency", [&] {
                sink_->set_center_freq(tx_hardware_center, stream_channel);
                const double applied = sink_->get_center_freq(stream_channel);
                require_precision(tx_hardware_center, applied, 0.5,
                                  "TX frequency");
                if (stream_channel == active_tx_stream_channel) {
                    applied_tx_center = applied;
                }
            });
            uhd_step("set TX gain", [&] {
                sink_->set_gain(tx_gain_db, stream_channel);
                const double applied = sink_->get_gain(stream_channel);
                require_precision(tx_gain_db, applied, 0.05, "TX gain");
                if (stream_channel == active_tx_stream_channel) {
                    applied_tx_gain = applied;
                }
            });
            uhd_step("set TX antenna", [&] {
                sink_->set_antenna(tx.antenna, stream_channel);
            });
            uhd_step("set TX bandwidth", [&] {
                sink_->set_bandwidth(static_cast<double>(tx.bandwidth_hz),
                                     stream_channel);
            });
        }

        flowgraph_ = gr::make_top_block("dmr_b210_rpt");
        flowgraph_->connect(source_, 0, channel_filter, 0);
        const auto signal_monitor = ReceiveSignalMonitor::make(
            "dmr_diagnostic_signal_monitor", squelch_dbfs,
            dmr_receive_metrics, true, rx_diagnostic_,
            [this, relay = relay_, channel = rx.channel,
             metrics = dmr_receive_metrics,
             threshold_dbfs = squelch_dbfs](double power) {
                relay->observe_dmr_signal_power(power);
                if (network_) {
                    network_->observe_signal(channel, power);
                    const ReceiveSignalSnapshot snapshot = metrics->snapshot();
                    const int32_t gain = current_rx_gain(channel);
                    if (calibration_) {
                        calibration_->observe(channel, gain, snapshot.signal_dbfs,
                                              snapshot.noise_dbfs,
                                              snapshot.snr_db, monotonic_ms(),
                                              snapshot.signal_dbfs &&
                                                  *snapshot.signal_dbfs >= threshold_dbfs);
                    }
                    const RxCalibrationReading reading = calibration_
                        ? calibration_->reading(channel, gain,
                                                snapshot.signal_dbfs)
                        : RxCalibrationReading{};
                    ReceiveStatus status;
                    status.rx_channel = channel;
                    status.receiver_mode = "dmr";
                    status.receiving = snapshot.signal_dbfs &&
                        *snapshot.signal_dbfs >= threshold_dbfs;
                    status.rssi_dbfs = snapshot.signal_dbfs;
                    status.rssi_dbm = reading.rssi_dbm;
                    status.snr_db = snapshot.snr_db;
                    status.calibration_state = reading.calibrated
                        ? "calibrated" : "uncalibrated";
                    network_->observe_receive_status(status);
                }
            });
        flowgraph_->connect(channel_filter, 0, signal_monitor, 0);
        flowgraph_->connect(signal_monitor, 0, squelch, 0);
        flowgraph_->connect(squelch, 0, dmr_agc, 0);
        flowgraph_->connect(dmr_agc, 0, fm_demod, 0);
        flowgraph_->connect(fm_demod, 0, rx_matched_filter, 0);
        flowgraph_->connect(rx_matched_filter, 0, symbol_sampler, 0);
        flowgraph_->connect(symbol_sampler, 0, frame_decoder, 0);
        flowgraph_->msg_connect(symbol_sampler, "bursts", relay_, "bursts");
        flowgraph_->msg_connect(frame_decoder, "frames", relay_, "frames");

        if (afm_enabled) {
            const auto afm_signal_monitor = ReceiveSignalMonitor::make(
                "fm_signal_monitor", afm.fm.squelch_tenths_dbfs / 10.0,
                fm_receive_metrics, false, false,
                [this, channel = afm.rx.channel,
                 metrics = fm_receive_metrics,
                 threshold_dbfs = afm.fm.squelch_tenths_dbfs / 10.0](
                    double power) {
                    if (network_) {
                        network_->observe_signal(channel, power);
                        const ReceiveSignalSnapshot snapshot =
                            metrics->snapshot();
                        const int32_t gain = current_rx_gain(channel);
                        if (calibration_) {
                            calibration_->observe(channel, gain,
                                                  snapshot.signal_dbfs,
                                                  snapshot.noise_dbfs,
                                                  snapshot.snr_db,
                                                  monotonic_ms(),
                                                  snapshot.signal_dbfs &&
                                                      *snapshot.signal_dbfs >= threshold_dbfs);
                        }
                        const RxCalibrationReading reading = calibration_
                            ? calibration_->reading(channel, gain,
                                                    snapshot.signal_dbfs)
                            : RxCalibrationReading{};
                        ReceiveStatus status;
                        status.rx_channel = channel;
                        status.receiver_mode = "fm";
                        status.receiving = snapshot.signal_dbfs &&
                            *snapshot.signal_dbfs >= threshold_dbfs;
                        status.rssi_dbfs = snapshot.signal_dbfs;
                        status.rssi_dbm = reading.rssi_dbm;
                        status.snr_db = snapshot.snr_db;
                        status.calibration_state = reading.calibrated
                            ? "calibrated" : "uncalibrated";
                        network_->observe_receive_status(status);
                    }
                });
            flowgraph_->connect(source_, 1, afm_channel_filter, 0);
            flowgraph_->connect(afm_channel_filter, 0, afm_signal_monitor, 0);
            flowgraph_->connect(afm_signal_monitor, 0, afm_squelch, 0);
            flowgraph_->connect(afm_squelch, 0, afm_agc, 0);
            flowgraph_->connect(afm_agc, 0, afm_demod, 0);
            flowgraph_->connect(afm_demod, 0, ctcss_decimator, 0);
            flowgraph_->connect(ctcss_decimator, 0, ctcss_monitor, 0);
            flowgraph_->connect(afm_demod, 0, voice_decimator, 0);
            flowgraph_->connect(voice_decimator, 0, afm_recording_sink, 0);
            flowgraph_->connect(voice_decimator, 0, afm_audio_scale, 0);
            flowgraph_->connect(afm_audio_scale, 0, afm_float_to_short, 0);
            flowgraph_->connect(afm_float_to_short, 0, afm_ambe_encoder, 0);
            flowgraph_->connect(afm_ambe_encoder, 0, relay_, 0);
            flowgraph_->connect(rx_matched_filter, 0, relay_, 1);
        } else {
            flowgraph_->connect(rx_matched_filter, 0, relay_, 0);
        }

        flowgraph_->connect(relay_, 0, tx_symbols, 0);
        flowgraph_->connect(tx_symbols, 0, tx_pulse, 0);
        flowgraph_->connect(tx_pulse, 0, tx_fm, 0);
        flowgraph_->connect(relay_, 1, tx_gate_repeat, 0);
        flowgraph_->connect(tx_gate_repeat, 0, tx_gate_delay, 0);
        flowgraph_->connect(tx_gate_delay, 0, tx_gate_complex, 0);
        flowgraph_->connect(tx_fm, 0, tx_gate, 0);
        flowgraph_->connect(tx_gate_complex, 0, tx_gate, 1);
        flowgraph_->connect(tx_gate, 0, tx_mixer, 0);
        flowgraph_->connect(tx_oscillator, 0, tx_mixer, 1);
        flowgraph_->connect(tx_mixer, 0, sink_, 0);
        if (mirrored_tx_streams) {
            flowgraph_->connect(tx_mixer, 0, sink_, 1);
        }

        flowgraph_->start();
        running_ = true;
        audit_.emit({"RF", "rf_session.started", "start", "ok",
                     config_.semantic_sha256,
                     {{"profile", to_string(config_.config.dmr.profile)},
                      {"rx_frequency_hz", std::to_string(rx.frequency_hz)},
                      {"tx_frequency_hz", std::to_string(tx.frequency_hz)},
                      {"rx_channel", std::to_string(rx.channel)},
                       {"tx_channel", std::to_string(tx.channel)},
                      {"tx_stream_channels", mirrored_tx_streams ? "0,1" :
                          std::to_string(tx.channel)},
                      {"tx_startup_prefill_ms", std::to_string(
                            kSingleStreamTxStartupPrefillFrames *
                                kDirectModeFrameDurationMs)},
                      {"applied_rx_center_hz", std::to_string(applied_rx_center)},
                      {"applied_tx_center_hz", std::to_string(applied_tx_center)},
                      {"applied_rx_gain_db", std::to_string(applied_rx_gain)},
                      {"applied_tx_gain_db", std::to_string(applied_tx_gain)},
                      {"receive_squelch_dbfs", std::to_string(squelch_dbfs)},
                      {"receive_agc_enabled",
                       config_.config.radio.receive_agc.enabled ? "true" : "false"},
                      {"receive_agc_target_dbfs", format_decimal(
                           config_.config.radio.receive_agc.target_tenths_dbfs / 10.0)},
                      {"receive_agc_gain_range_db", format_decimal(
                           config_.config.radio.receive_agc.minimum_gain_tenths_db / 10.0) +
                           ":" + format_decimal(
                               config_.config.radio.receive_agc.maximum_gain_tenths_db / 10.0)},
                      {"receive_agc_attack_db_per_second", format_decimal(
                           config_.config.radio.receive_agc.attack_tenths_db_per_second / 10.0)},
                      {"receive_agc_release_db_per_second", format_decimal(
                           config_.config.radio.receive_agc.release_tenths_db_per_second / 10.0)},
                      {"analog_fm_enabled", afm_enabled ? "true" : "false"},
                      {"analog_fm_rx_channel", std::to_string(afm.rx.channel)},
                      {"analog_fm_applied_rx_center_hz",
                       std::to_string(applied_afm_rx_center)},
                      {"analog_fm_applied_rx_gain_db",
                       std::to_string(applied_afm_rx_gain)},
                      {"analog_fm_ctcss_hz", format_decimal(
                           afm.ctcss.tone_tenths_hz / 10.0)},
                      {"recording_directory",
                        config_.config.logging.recording_directory.string()},
                       {"rx_diagnostic", rx_diagnostic_ ? "true" : "false"}}});
    }

    void stop() override
    {
        if (!running_) {
            if (recording_) recording_->stop();
            if (io_) io_->release();
            return;
        }
        if (flowgraph_) {
            flowgraph_->stop();
            flowgraph_->wait();
        }
        if (recording_) {
            recording_->stop();
        }
        if (io_) {
            io_->release();
        }
        running_ = false;
        const RecordingRuntimeStats recording_stats = recording_
            ? recording_->stats()
            : RecordingRuntimeStats{};
        audit_.emit({"RF", "rf_session.stopped", "stop", "ok",
                     config_.semantic_sha256,
                     {{"accepted_bursts", std::to_string(relay_->accepted_bursts())},
                      {"transmitted_bursts", std::to_string(relay_->transmitted_bursts())},
                      {"rejected_frames", std::to_string(relay_->rejected_frames())},
                      {"analog_fm_sessions", std::to_string(
                           relay_->analog_fm_sessions())},
                      {"analog_fm_voice_bursts", std::to_string(
                           relay_->analog_fm_voice_bursts())},
                      {"analog_fm_dropped_ambe_frames", std::to_string(
                           relay_->analog_fm_dropped_ambe_frames())},
                      {"recording_completed_calls", std::to_string(
                           recording_stats.completed_calls)},
                      {"recording_failed_calls", std::to_string(
                           recording_stats.failed_calls)},
                      {"recording_audio_frames", std::to_string(
                           recording_stats.audio_frames)},
                      {"recording_dropped_frames", std::to_string(
                           recording_stats.dropped_frames)}}});
    }

    void poll(std::int64_t now_ms) override
    {
        if (io_) {
            io_->poll(now_ms);
        }
        if (now_ms - last_agc_audit_at_ms_ < 1000) {
            return;
        }
        last_agc_audit_at_ms_ = now_ms;
        const auto telemetry_value = [](const std::shared_ptr<ReceiveAgcTelemetry>& item,
                                        std::atomic<int> ReceiveAgcTelemetry::*field) {
            return item ? format_decimal((item.get()->*field).load(
                       std::memory_order_relaxed) / 10.0) : std::string{"--.-"};
        };
        audit_.emit({"RF", "rf_receive_agc.status", "monitor", "ok",
                     config_.semantic_sha256,
                     {{"dmr_input_dbfs", telemetry_value(
                           dmr_agc_telemetry_, &ReceiveAgcTelemetry::input_tenths_dbfs)},
                      {"dmr_gain_db", telemetry_value(
                           dmr_agc_telemetry_, &ReceiveAgcTelemetry::gain_tenths_db)},
                      {"fm_input_dbfs", telemetry_value(
                           fm_agc_telemetry_, &ReceiveAgcTelemetry::input_tenths_dbfs)},
                      {"fm_gain_db", telemetry_value(
                           fm_agc_telemetry_, &ReceiveAgcTelemetry::gain_tenths_db)}}});
    }

    bool health_check(std::string& error) const override
    {
        if (!running_ || !source_ || !sink_) {
            error = "B210 UHD session is not running";
            return false;
        }
        try {
            const double rx_rate = source_->get_samp_rate();
            const double tx_rate = sink_->get_samp_rate();
            if (std::abs(rx_rate - kUsrpRate) > 0.5 ||
                std::abs(tx_rate - kUsrpRate) > 0.5) {
                error = "B210 UHD sample-rate health check failed";
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            error = std::string("B210 UHD health check failed: ") + exception.what();
            return false;
        }
    }

    bool set_rx_gain(int physical_rx_channel, std::int32_t gain_tenths_db,
                     std::string& error) override
    {
        std::lock_guard<std::mutex> lock(rx_config_mutex_);
        const auto stream = rx_stream_channels_.find(physical_rx_channel);
        if (!source_ || stream == rx_stream_channels_.end()) {
            error = "RX channel is not active";
            return false;
        }
        try {
            const double requested = gain_tenths_db / 10.0;
            source_->set_gain(requested, stream->second);
            const double applied = source_->get_gain(stream->second);
            require_precision(requested, applied, 0.05, "RX gain");
            rx_gain_tenths_db_[physical_rx_channel] = gain_tenths_db;
            return true;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }
    }

    std::optional<RxCalibrationObservation> calibration_observation(
        int physical_rx_channel) const override
    {
        return calibration_ ? calibration_->observation(physical_rx_channel)
                            : std::nullopt;
    }

private:
    std::int32_t current_rx_gain(int physical_rx_channel) const
    {
        std::lock_guard<std::mutex> lock(rx_config_mutex_);
        const auto found = rx_gain_tenths_db_.find(physical_rx_channel);
        return found == rx_gain_tenths_db_.end() ? 0 : found->second;
    }

    void handle_recording_notice(const RecordingNotice& notice)
    {
        audit_.emit({"REC", notice.event_type, "record_audio", notice.result,
                     notice.metadata.correlation_id,
                     {{"mode", to_string(notice.metadata.mode)},
                      {"source_id", std::to_string(notice.metadata.source_id)},
                      {"destination_id", std::to_string(
                           notice.metadata.destination_id)},
                      {"color_code", std::to_string(
                           notice.metadata.color_code)},
                      {"slot", std::to_string(notice.metadata.slot)},
                      {"recording_path", notice.path.string()},
                      {"reason", notice.reason},
                      {"error", notice.error},
                      {"audio_frames", std::to_string(notice.audio_frames)},
                      {"dropped_frames", std::to_string(
                           notice.dropped_frames)}}});

        std::lock_guard<std::mutex> lock(recording_console_mutex_);
        if (notice.event_type == "recording.started") {
            write_console_message(
                std::cout, std::string(console_token::RecordingStart) + ' ' +
                               to_string(notice.metadata.mode));
        } else if (notice.event_type == "recording.completed") {
            write_console_message(
                std::cout, std::string(console_token::RecordingStop) + ' ' +
                               to_string(notice.metadata.mode));
            if (recording_storage_update_) recording_storage_update_();
        } else {
            write_console_message(
                std::cerr, std::string(console_token::RecordingFail) + ' ' +
                               to_string(notice.metadata.mode));
        }
    }

    ValidatedConfig config_;
    OperationAuditLogger& audit_;
    gr::top_block_sptr flowgraph_;
    gr::uhd::usrp_source::sptr source_;
    gr::uhd::usrp_sink::sptr sink_;
    DirectRelayBurstSource::sptr relay_;
    std::shared_ptr<AudioRecordingRuntime> recording_;
    std::shared_ptr<ReceiveAgcTelemetry> dmr_agc_telemetry_;
    std::shared_ptr<ReceiveAgcTelemetry> fm_agc_telemetry_;
    std::unique_ptr<GnuradioB210GpioAdapter> gpio_;
    std::unique_ptr<RuntimeIo> io_;
    std::shared_ptr<NetworkEventSink> network_;
    std::shared_ptr<std::atomic_bool> forwarding_enabled_;
    std::shared_ptr<RxSignalCalibrationRuntime> calibration_;
    std::function<void()> recording_storage_update_;
    mutable std::mutex rx_config_mutex_;
    std::map<int, std::size_t> rx_stream_channels_;
    std::map<int, std::int32_t> rx_gain_tenths_db_;
    std::mutex recording_console_mutex_;
    bool rx_diagnostic_ = false;
    bool running_ = false;
    std::int64_t last_agc_audit_at_ms_ = 0;
};

} // namespace

HardwareB210SessionFactory::HardwareB210SessionFactory(
    ValidatedConfig config, OperationAuditLogger& audit, bool rx_diagnostic,
    std::shared_ptr<NetworkEventSink> network,
    std::shared_ptr<std::atomic_bool> forwarding_enabled,
    std::shared_ptr<RxSignalCalibrationRuntime> calibration,
    std::function<void()> recording_storage_update)
    : config_(std::move(config)), audit_(audit),
      rx_diagnostic_(rx_diagnostic),
      network_(std::move(network)),
      forwarding_enabled_(std::move(forwarding_enabled)),
      calibration_(std::move(calibration)),
      recording_storage_update_(std::move(recording_storage_update))
{
}

std::unique_ptr<B210Session> HardwareB210SessionFactory::create()
{
    return std::make_unique<HardwareB210Session>(
        config_, audit_, rx_diagnostic_, network_, forwarding_enabled_,
        calibration_, recording_storage_update_);
}

} // namespace dmr_rpt
