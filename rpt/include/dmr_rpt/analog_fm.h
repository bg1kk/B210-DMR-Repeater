// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dmr_rpt/config.h"

namespace dmr_rpt {

inline constexpr std::size_t kAnalogFmMaxQueuedAmbeFrames = 9U;
inline constexpr unsigned kAnalogFmRelayHeaderBursts = 2U;
inline constexpr int kAnalogFmDmrBurstDurationMs = 60;
inline constexpr int kAnalogFmCtcssPhaseQualificationMs = 40;
inline constexpr int kAnalogFmAmbePrefillMs = 60;

constexpr int analog_fm_relay_start_latency_bound_ms(
    const AnalogFmFallbackConfig& config)
{
    return config.ctcss.minimum_detect_ms + config.dmr_idle_guard_ms +
        kAnalogFmCtcssPhaseQualificationMs + kAnalogFmAmbePrefillMs +
        static_cast<int>(kAnalogFmRelayHeaderBursts) *
            kAnalogFmDmrBurstDurationMs;
}

constexpr bool analog_fm_can_qualify(bool ctcss_qualified,
                                     bool rearm_required,
                                     bool dmr_active)
{
    return ctcss_qualified && !rearm_required && !dmr_active;
}

struct CtcssState {
    double configured_tone_hz = 123.0;
    double confidence_db = 0.0;
    bool qualified = false;
    std::int64_t qualified_since_sample = -1;
};

class CtcssDetector {
public:
    explicit CtcssDetector(CtcssConfig config, int sample_rate_hz = 8000);

    CtcssState process(const float* samples, std::size_t count);
    CtcssState process(const std::vector<float>& samples);
    void reset();
    const CtcssState& state() const;

private:
    void push(float sample);
    void evaluate_window();
    std::pair<double, double> coherent_components(double frequency_hz) const;
    double coherent_power(double frequency_hz) const;

    CtcssConfig config_;
    int sample_rate_hz_ = 8000;
    std::size_t analysis_samples_ = 0;
    std::size_t hop_samples_ = 0;
    std::vector<float> ring_;
    std::size_t write_index_ = 0;
    std::size_t buffered_samples_ = 0;
    std::size_t samples_since_evaluation_ = 0;
    std::int64_t total_samples_ = 0;
    std::int64_t detection_samples_ = 0;
    std::int64_t release_samples_ = 0;
    bool have_target_phase_ = false;
    double last_target_phase_ = 0.0;
    unsigned coherent_target_windows_ = 0;
    CtcssState state_;
};

} // namespace dmr_rpt
