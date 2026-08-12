// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/interlock.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace dmr_rpt {

DmrSourceCooldownStore::DmrSourceCooldownStore(std::filesystem::path persistence_path)
    : persistence_path_(std::move(persistence_path))
{
}

void DmrSourceCooldownStore::load(std::int64_t now_ms,
                                  int conservative_cooldown_seconds)
{
    records_.clear();
    if (persistence_path_.empty() || !std::filesystem::exists(persistence_path_)) {
        return;
    }
    std::ifstream input(persistence_path_);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::uint32_t source_id = 0;
        std::int64_t expires_at_ms = 0;
        if (row >> source_id >> expires_at_ms) {
            if (expires_at_ms <= now_ms) {
                expires_at_ms = now_ms +
                    static_cast<std::int64_t>(conservative_cooldown_seconds) * 1000;
            }
            records_[source_id] = {source_id, expires_at_ms,
                                   "MaximumRelayDurationReached"};
        }
    }
}

void DmrSourceCooldownStore::record_duration_limit(std::uint32_t source_id,
                                                   std::int64_t now_ms,
                                                   int cooldown_seconds)
{
    records_[source_id] = {
        source_id,
        now_ms + static_cast<std::int64_t>(cooldown_seconds) * 1000,
        "MaximumRelayDurationReached"
    };
    persist();
}

SourceCooldownInfo DmrSourceCooldownStore::check(std::uint32_t source_id,
                                                 std::int64_t now_ms) const
{
    const auto found = records_.find(source_id);
    if (found == records_.end() || found->second.expires_at_ms <= now_ms) {
        return {};
    }
    return {true, found->second.expires_at_ms - now_ms};
}

std::vector<DmrSourceCooldown> DmrSourceCooldownStore::active(std::int64_t now_ms) const
{
    std::vector<DmrSourceCooldown> result;
    for (const auto& item : records_) {
        if (item.second.expires_at_ms > now_ms) {
            result.push_back(item.second);
        }
    }
    return result;
}

void DmrSourceCooldownStore::persist() const
{
    if (persistence_path_.empty()) {
        return;
    }
    if (!persistence_path_.parent_path().empty()) {
        std::filesystem::create_directories(persistence_path_.parent_path());
    }
    std::filesystem::path temp = persistence_path_;
    temp += ".tmp";
    std::ofstream output(temp, std::ios::trunc);
    for (const auto& item : records_) {
        output << item.second.source_id << ' ' << item.second.expires_at_ms << '\n';
    }
    output.close();
    std::filesystem::rename(temp, persistence_path_);
}

void ConfigurationRebuildInterlock::begin(std::string activation_id)
{
    inhibited_ = true;
    activation_id_ = std::move(activation_id);
}

void ConfigurationRebuildInterlock::complete(bool success)
{
    if (success) {
        inhibited_ = false;
        activation_id_.clear();
    }
}

bool ConfigurationRebuildInterlock::inhibited() const
{
    return inhibited_;
}

const std::string& ConfigurationRebuildInterlock::activation_id() const
{
    return activation_id_;
}

AutomaticTransmitGate::AutomaticTransmitGate(TransmitConfig config)
    : config_(config)
{
}

AutomaticTransmitGrant AutomaticTransmitGate::evaluate(const TxRequest& request,
                                                       const RfHealthState& rf_health,
                                                       std::int64_t now_ms) const
{
    AutomaticTransmitGrant grant;
    grant.request = request;
    if (rebuild_inhibit_) {
        grant.reason = RouteRejectReason::ConfigurationRebuildInhibit;
        return grant;
    }
    if (!config_.enabled || maintenance_inhibit_) {
        grant.reason = RouteRejectReason::TxDisabled;
        return grant;
    }
    if (!rf_health.healthy || !rf_health.frequency_in_range) {
        grant.reason = RouteRejectReason::InvalidRequest;
        return grant;
    }
    if (!rf_health.slot_available) {
        grant.reason = RouteRejectReason::ResourceBusy;
        return grant;
    }
    grant.granted = true;
    grant.reason = RouteRejectReason::None;
    grant.expires_at_ms = now_ms +
        static_cast<std::int64_t>(config_.maximum_continuous_seconds) * 1000;
    return grant;
}

void AutomaticTransmitGate::set_transmit_enabled(bool enabled)
{
    config_.enabled = enabled;
}

void AutomaticTransmitGate::set_maintenance_inhibit(bool inhibited)
{
    maintenance_inhibit_ = inhibited;
}

void AutomaticTransmitGate::set_rebuild_inhibit(bool inhibited)
{
    rebuild_inhibit_ = inhibited;
}

} // namespace dmr_rpt
