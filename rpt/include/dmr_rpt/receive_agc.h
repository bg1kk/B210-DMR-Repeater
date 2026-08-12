// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "dmr_rpt/config.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace dmr_rpt {

struct ReceiveAgcSnapshot {
    std::optional<double> input_dbfs;
    double gain_db = 0.0;
    std::optional<double> output_dbfs;
};

class ReceiveAgcController {
public:
    explicit ReceiveAgcController(ReceiveAgcConfig config)
        : config_(config)
    {
    }

    void observe_average_power(
        double power, double observation_seconds,
        std::optional<double> activation_threshold_dbfs = std::nullopt)
    {
        if (!config_.enabled || power <= kSilencePower ||
            observation_seconds <= 0.0) {
            return;
        }
        input_dbfs_ = 10.0 * std::log10(std::max(power, kSilencePower));
        if (activation_threshold_dbfs &&
            *input_dbfs_ < *activation_threshold_dbfs) {
            gain_db_ = 0.0;
            return;
        }
        const double requested_gain_db = std::clamp(
            config_.target_tenths_dbfs / 10.0 - *input_dbfs_,
            config_.minimum_gain_tenths_db / 10.0,
            config_.maximum_gain_tenths_db / 10.0);
        const double rate_db_per_second = requested_gain_db < gain_db_
            ? config_.attack_tenths_db_per_second / 10.0
            : config_.release_tenths_db_per_second / 10.0;
        const double change_limit = rate_db_per_second * observation_seconds;
        gain_db_ += std::clamp(requested_gain_db - gain_db_,
                               -change_limit, change_limit);
    }

    void reset()
    {
        input_dbfs_.reset();
        gain_db_ = 0.0;
    }

    bool enabled() const
    {
        return config_.enabled;
    }

    double gain_linear() const
    {
        return std::pow(10.0, gain_db_ / 20.0);
    }

    ReceiveAgcSnapshot snapshot() const
    {
        return {input_dbfs_, gain_db_,
                input_dbfs_
                    ? std::optional<double>(*input_dbfs_ + gain_db_)
                    : std::nullopt};
    }

private:
    static constexpr double kSilencePower = 1e-16;
    ReceiveAgcConfig config_;
    std::optional<double> input_dbfs_;
    double gain_db_ = 0.0;
};

} // namespace dmr_rpt
