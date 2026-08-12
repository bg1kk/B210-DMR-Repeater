// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/analog_fm.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dmr_rpt {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kAnalysisWindowMs = 250;
constexpr int kEvaluationHopMs = 20;
constexpr double kProbeOffsetHz = 4.0;
constexpr double kMinimumSignalPower = 1.0e-8;
constexpr unsigned kMinimumCoherentWindows = 3U;
constexpr double kMaximumPhaseErrorRadians = 0.55;

std::int64_t milliseconds_to_samples(int milliseconds, int sample_rate_hz)
{
    return std::max<std::int64_t>(
        1, (static_cast<std::int64_t>(milliseconds) * sample_rate_hz + 999) / 1000);
}

} // namespace

CtcssDetector::CtcssDetector(CtcssConfig config, int sample_rate_hz)
    : config_(config), sample_rate_hz_(sample_rate_hz)
{
    if (sample_rate_hz_ <= 0 || config_.tone_tenths_hz <= 0 ||
        config_.minimum_detect_ms <= 0 || config_.release_hold_ms < 0) {
        throw std::invalid_argument("invalid CTCSS detector configuration");
    }
    analysis_samples_ = static_cast<std::size_t>(
        milliseconds_to_samples(kAnalysisWindowMs, sample_rate_hz_));
    hop_samples_ = static_cast<std::size_t>(
        milliseconds_to_samples(kEvaluationHopMs, sample_rate_hz_));
    ring_.assign(analysis_samples_, 0.0F);
    state_.configured_tone_hz = config_.tone_tenths_hz / 10.0;
}

CtcssState CtcssDetector::process(const float* samples, std::size_t count)
{
    if (samples == nullptr && count != 0U) {
        throw std::invalid_argument("CTCSS sample buffer is null");
    }
    for (std::size_t index = 0; index < count; ++index) {
        push(samples[index]);
    }
    return state_;
}

CtcssState CtcssDetector::process(const std::vector<float>& samples)
{
    return process(samples.data(), samples.size());
}

void CtcssDetector::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    write_index_ = 0;
    buffered_samples_ = 0;
    samples_since_evaluation_ = 0;
    total_samples_ = 0;
    detection_samples_ = 0;
    release_samples_ = 0;
    have_target_phase_ = false;
    last_target_phase_ = 0.0;
    coherent_target_windows_ = 0;
    state_.confidence_db = 0.0;
    state_.qualified = false;
    state_.qualified_since_sample = -1;
}

const CtcssState& CtcssDetector::state() const
{
    return state_;
}

void CtcssDetector::push(float sample)
{
    ring_[write_index_] = sample;
    write_index_ = (write_index_ + 1U) % ring_.size();
    buffered_samples_ = std::min(buffered_samples_ + 1U, ring_.size());
    ++samples_since_evaluation_;
    ++total_samples_;

    if (buffered_samples_ == ring_.size() &&
        samples_since_evaluation_ >= hop_samples_) {
        samples_since_evaluation_ = 0;
        evaluate_window();
    }
}

std::pair<double, double> CtcssDetector::coherent_components(
    double frequency_hz) const
{
    double in_phase = 0.0;
    double quadrature = 0.0;
    const double window_sum = static_cast<double>(ring_.size());
    for (std::size_t index = 0; index < ring_.size(); ++index) {
        const std::size_t position = (write_index_ + index) % ring_.size();
        const double phase = 2.0 * kPi * frequency_hz *
            static_cast<double>(index) / static_cast<double>(sample_rate_hz_);
        const double value = static_cast<double>(ring_[position]);
        in_phase += value * std::cos(phase);
        quadrature += value * std::sin(phase);
    }
    if (window_sum <= std::numeric_limits<double>::epsilon()) {
        return {0.0, 0.0};
    }
    return {in_phase / window_sum, quadrature / window_sum};
}

double CtcssDetector::coherent_power(double frequency_hz) const
{
    const auto [in_phase, quadrature] = coherent_components(frequency_hz);
    return in_phase * in_phase + quadrature * quadrature;
}

void CtcssDetector::evaluate_window()
{
    const double target_hz = state_.configured_tone_hz;
    const auto [target_in_phase, target_quadrature] =
        coherent_components(target_hz);
    const double target_power = target_in_phase * target_in_phase +
        target_quadrature * target_quadrature;
    const double lower_power = coherent_power(std::max(1.0, target_hz - kProbeOffsetHz));
    const double upper_power = coherent_power(target_hz + kProbeOffsetHz);
    const double reference_power = std::max(
        {lower_power, upper_power, kMinimumSignalPower});
    state_.confidence_db = 10.0 * std::log10(
        std::max(target_power, kMinimumSignalPower) / reference_power);

    const bool spectrally_detected = target_power >= kMinimumSignalPower &&
        state_.confidence_db >= config_.minimum_confidence_tenths_db / 10.0;
    if (spectrally_detected) {
        const double phase = std::atan2(target_quadrature, target_in_phase);
        if (!have_target_phase_) {
            have_target_phase_ = true;
            last_target_phase_ = phase;
            coherent_target_windows_ = 1U;
            detection_samples_ = 0;
            return;
        }
        const double expected_advance = 2.0 * kPi * target_hz *
            static_cast<double>(hop_samples_) /
            static_cast<double>(sample_rate_hz_);
        const double phase_error = std::abs(std::remainder(
            phase - last_target_phase_ + expected_advance, 2.0 * kPi));
        last_target_phase_ = phase;
        if (phase_error <= kMaximumPhaseErrorRadians) {
            ++coherent_target_windows_;
        } else {
            coherent_target_windows_ = 1U;
            detection_samples_ = 0;
        }
        if (coherent_target_windows_ < kMinimumCoherentWindows) {
            return;
        }
        release_samples_ = 0;
        detection_samples_ = std::max<std::int64_t>(
            detection_samples_ + static_cast<std::int64_t>(hop_samples_),
            static_cast<std::int64_t>(analysis_samples_));
        const std::int64_t required = milliseconds_to_samples(
            config_.minimum_detect_ms, sample_rate_hz_);
        if (!state_.qualified && detection_samples_ >= required) {
            state_.qualified = true;
            state_.qualified_since_sample = total_samples_ - detection_samples_;
        }
        return;
    }

    detection_samples_ = 0;
    have_target_phase_ = false;
    coherent_target_windows_ = 0;
    if (!state_.qualified) {
        return;
    }
    release_samples_ += static_cast<std::int64_t>(hop_samples_);
    const std::int64_t release_required = milliseconds_to_samples(
        config_.release_hold_ms, sample_rate_hz_);
    if (release_samples_ >= release_required) {
        state_.qualified = false;
        state_.qualified_since_sample = -1;
        release_samples_ = 0;
    }
}

} // namespace dmr_rpt
