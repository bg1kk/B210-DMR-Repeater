// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <memory>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "dmr_rpt/config.h"
#include "dmr_rpt/rx_signal_calibration.h"

namespace dmr_rpt {

class B210Session {
public:
    virtual ~B210Session() = default;
    virtual void start(const ValidatedRfConfig& config) = 0;
    virtual void poll(std::int64_t now_ms) { (void)now_ms; }
    virtual void stop() = 0;
    virtual bool set_rx_gain(int physical_rx_channel,
                             std::int32_t gain_tenths_db,
                             std::string& error)
    {
        (void)physical_rx_channel;
        (void)gain_tenths_db;
        error = "RX gain control is unavailable";
        return false;
    }
    virtual std::optional<RxCalibrationObservation>
    calibration_observation(int physical_rx_channel) const
    {
        (void)physical_rx_channel;
        return std::nullopt;
    }
};

class B210SessionFactory {
public:
    virtual ~B210SessionFactory() = default;
    virtual std::unique_ptr<B210Session> create() = 0;
};

struct RfReinitializationResult {
    bool activated = false;
    bool rolled_back = false;
    std::string phase;
    std::string error;
};

class RfReinitializationController {
public:
    explicit RfReinitializationController(B210SessionFactory& factory);

    RfReinitializationResult start_initial(const ValidatedRfConfig& initial);
    RfReinitializationResult reinitialize(const ValidatedRfConfig& candidate,
                                          const ValidatedRfConfig& previous);
    void poll(std::int64_t now_ms);
    void stop();
    bool running() const;
    bool set_rx_gain(int physical_rx_channel, std::int32_t gain_tenths_db,
                     std::string& error);
    std::optional<RxCalibrationObservation>
    calibration_observation(int physical_rx_channel) const;

private:
    B210SessionFactory& factory_;
    std::unique_ptr<B210Session> current_;
};

class DryRunB210Session final : public B210Session {
public:
    void start(const ValidatedRfConfig& config) override;
    void stop() override;
    bool started() const;

private:
    bool started_ = false;
};

class DryRunB210SessionFactory final : public B210SessionFactory {
public:
    std::unique_ptr<B210Session> create() override;
    int create_count() const;

private:
    int create_count_ = 0;
};

} // namespace dmr_rpt
