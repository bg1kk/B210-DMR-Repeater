// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

namespace dmr_rpt {

struct ReceiveSignalSnapshot {
    std::optional<double> signal_dbfs;
    std::optional<double> noise_dbfs;
    std::optional<double> snr_db;
};

enum class DmrAdmissionTimeoutReason {
    NoReliableSync,
    MissingLinkControl,
};

class ReceiveSignalMetrics {
public:
    explicit ReceiveSignalMetrics(double activity_threshold_dbfs,
                                  std::int64_t signal_release_hold_ms = 0)
        : activity_threshold_dbfs_(activity_threshold_dbfs)
        , signal_release_hold_ms_(
              std::max<std::int64_t>(0, signal_release_hold_ms))
    {
    }

    void observe_average_power(double power)
    {
        observe_average_power(power, monotonic_ms());
    }

    void observe_average_power(double power, std::int64_t observed_at_ms)
    {
        const double signal_dbfs = 10.0 * std::log10(std::max(power, 1e-20));
        std::lock_guard<std::mutex> lock(mutex_);
        signal_dbfs_ = signal_dbfs;
        if (signal_dbfs >= activity_threshold_dbfs_) {
            const bool previous_signal_released =
                last_active_observation_at_ms_ &&
                observed_at_ms >= *last_active_observation_at_ms_ &&
                observed_at_ms - *last_active_observation_at_ms_ >=
                    signal_release_hold_ms_;
            if (!active_signal_dbfs_ || previous_signal_released) {
                active_signal_dbfs_ = signal_dbfs;
            } else {
                constexpr double kActiveTrackingAlpha = 0.2;
                *active_signal_dbfs_ += kActiveTrackingAlpha *
                    (signal_dbfs - *active_signal_dbfs_);
            }
            last_active_observation_at_ms_ = observed_at_ms;
        }
        if (signal_dbfs < activity_threshold_dbfs_) {
            if (!noise_dbfs_) {
                noise_dbfs_ = signal_dbfs;
            } else {
                constexpr double kNoiseTrackingAlpha = 0.05;
                *noise_dbfs_ +=
                    kNoiseTrackingAlpha * (signal_dbfs - *noise_dbfs_);
            }
        }
    }

    ReceiveSignalSnapshot snapshot() const
    {
        return snapshot(monotonic_ms());
    }

    ReceiveSignalSnapshot snapshot(std::int64_t observed_at_ms) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool active_signal_held =
            signal_release_hold_ms_ > 0 && active_signal_dbfs_ &&
            last_active_observation_at_ms_ &&
            (observed_at_ms < *last_active_observation_at_ms_ ||
             observed_at_ms - *last_active_observation_at_ms_ <
                 signal_release_hold_ms_);
        const std::optional<double> effective_signal_dbfs =
            active_signal_held ? active_signal_dbfs_ : signal_dbfs_;
        ReceiveSignalSnapshot result{
            effective_signal_dbfs, noise_dbfs_, std::nullopt};
        if (effective_signal_dbfs && noise_dbfs_) {
            result.snr_db =
                std::max(0.0, *effective_signal_dbfs - *noise_dbfs_);
        }
        return result;
    }

    double activity_threshold_dbfs() const
    {
        return activity_threshold_dbfs_;
    }

private:
    static std::int64_t monotonic_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double activity_threshold_dbfs_;
    std::int64_t signal_release_hold_ms_ = 0;
    mutable std::mutex mutex_;
    std::optional<double> signal_dbfs_;
    std::optional<double> noise_dbfs_;
    std::optional<double> active_signal_dbfs_;
    std::optional<std::int64_t> last_active_observation_at_ms_;
};

class DmrAdmissionTracker {
public:
    DmrAdmissionTracker(double activity_threshold_dbfs,
                        std::int64_t decision_timeout_ms,
                        std::int64_t release_hold_ms = 200)
        : activity_threshold_dbfs_(activity_threshold_dbfs)
        , decision_timeout_ms_(std::max<std::int64_t>(0, decision_timeout_ms))
        , release_hold_ms_(std::max<std::int64_t>(0, release_hold_ms))
    {
    }

    void observe_average_power(double power, std::int64_t now_ms)
    {
        const double signal_dbfs =
            10.0 * std::log10(std::max(power, 1e-20));
        std::lock_guard<std::mutex> lock(mutex_);
        if (signal_dbfs >= activity_threshold_dbfs_) {
            if (!signal_active_) {
                signal_active_ = true;
                signal_started_at_ms_ = now_ms;
                sync_seen_ = false;
                resolved_ = false;
            }
            last_above_threshold_at_ms_ = now_ms;
            return;
        }
        if (signal_active_ &&
            now_ms - last_above_threshold_at_ms_ >= release_hold_ms_) {
            reset_locked();
        }
    }

    void mark_sync()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (signal_active_ && !resolved_) {
            sync_seen_ = true;
        }
    }

    void mark_admitted()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (signal_active_) {
            resolved_ = true;
        }
    }

    bool mark_failure()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!signal_active_ || resolved_) {
            return false;
        }
        resolved_ = true;
        return true;
    }

    std::optional<DmrAdmissionTimeoutReason> poll(std::int64_t now_ms)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!signal_active_ || resolved_ ||
            now_ms < signal_started_at_ms_ ||
            now_ms - signal_started_at_ms_ < decision_timeout_ms_) {
            return std::nullopt;
        }
        resolved_ = true;
        return sync_seen_
            ? DmrAdmissionTimeoutReason::MissingLinkControl
            : DmrAdmissionTimeoutReason::NoReliableSync;
    }

private:
    void reset_locked()
    {
        signal_active_ = false;
        sync_seen_ = false;
        resolved_ = false;
        signal_started_at_ms_ = 0;
        last_above_threshold_at_ms_ = 0;
    }

    double activity_threshold_dbfs_;
    std::int64_t decision_timeout_ms_;
    std::int64_t release_hold_ms_;
    mutable std::mutex mutex_;
    bool signal_active_ = false;
    bool sync_seen_ = false;
    bool resolved_ = false;
    std::int64_t signal_started_at_ms_ = 0;
    std::int64_t last_above_threshold_at_ms_ = 0;
};

} // namespace dmr_rpt
