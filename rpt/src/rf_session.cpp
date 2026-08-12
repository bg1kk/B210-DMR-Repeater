// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/rf_session.h"

#include <utility>

namespace dmr_rpt {

RfReinitializationController::RfReinitializationController(B210SessionFactory& factory)
    : factory_(factory)
{
}

RfReinitializationResult RfReinitializationController::start_initial(
    const ValidatedRfConfig& initial)
{
    RfReinitializationResult result;
    result.phase = "StartingCandidate";
    try {
        std::unique_ptr<B210Session> session = factory_.create();
        session->start(initial);
        current_ = std::move(session);
        result.activated = true;
        result.phase = "Activated";
    } catch (const std::exception& error) {
        result.phase = "Fault";
        result.error = error.what();
    }
    return result;
}

RfReinitializationResult RfReinitializationController::reinitialize(
    const ValidatedRfConfig& candidate,
    const ValidatedRfConfig& previous)
{
    RfReinitializationResult result;
    result.phase = "StartingCandidate";
    std::unique_ptr<B210Session> old = std::move(current_);
    try {
        if (old) {
            old->stop();
            // Release UHD/GNU Radio stream ownership before opening a candidate
            // that may use a different B210 RX channel set.
            old.reset();
        }
        std::unique_ptr<B210Session> next = factory_.create();
        next->start(candidate);
        current_ = std::move(next);
        result.activated = true;
        result.phase = "Activated";
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        result.phase = "RollingBack";
    }

    try {
        std::unique_ptr<B210Session> rollback = factory_.create();
        rollback->start(previous);
        current_ = std::move(rollback);
        result.rolled_back = true;
        result.phase = "RollingBack";
    } catch (const std::exception& rollback_error) {
        result.phase = "Fault";
        result.error += "; rollback failed: ";
        result.error += rollback_error.what();
    }
    return result;
}

void RfReinitializationController::poll(std::int64_t now_ms)
{
    if (current_) {
        current_->poll(now_ms);
    }
}

void RfReinitializationController::stop()
{
    if (current_) {
        current_->stop();
        current_.reset();
    }
}

bool RfReinitializationController::running() const
{
    return current_ != nullptr;
}

bool RfReinitializationController::set_rx_gain(
    int physical_rx_channel, std::int32_t gain_tenths_db, std::string& error)
{
    if (!current_) {
        error = "RF session is stopped";
        return false;
    }
    return current_->set_rx_gain(physical_rx_channel, gain_tenths_db, error);
}

std::optional<RxCalibrationObservation>
RfReinitializationController::calibration_observation(
    int physical_rx_channel) const
{
    return current_ ? current_->calibration_observation(physical_rx_channel)
                    : std::nullopt;
}

void DryRunB210Session::start(const ValidatedRfConfig& config)
{
    if (config.active_channel_profile_id.empty()) {
        throw std::runtime_error("dry-run B210 session received no active profile");
    }
    started_ = true;
}

void DryRunB210Session::stop()
{
    started_ = false;
}

bool DryRunB210Session::started() const
{
    return started_;
}

std::unique_ptr<B210Session> DryRunB210SessionFactory::create()
{
    ++create_count_;
    return std::make_unique<DryRunB210Session>();
}

int DryRunB210SessionFactory::create_count() const
{
    return create_count_;
}

} // namespace dmr_rpt
