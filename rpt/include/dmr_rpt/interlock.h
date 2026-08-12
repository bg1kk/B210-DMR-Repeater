// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dmr_rpt/config.h"
#include "dmr_rpt/event.h"

namespace dmr_rpt {

struct RfHealthState {
    bool healthy = true;
    bool frequency_in_range = true;
    bool slot_available = true;
    std::string last_error;
};

struct AutomaticTransmitGrant {
    bool granted = false;
    RouteRejectReason reason = RouteRejectReason::None;
    TxRequest request;
    std::int64_t expires_at_ms = 0;
};

struct DmrSourceCooldown {
    std::uint32_t source_id = 0;
    std::int64_t expires_at_ms = 0;
    std::string trigger = "MaximumRelayDurationReached";
};

class DmrSourceCooldownStore {
public:
    DmrSourceCooldownStore() = default;
    explicit DmrSourceCooldownStore(std::filesystem::path persistence_path);

    void load(std::int64_t now_ms, int conservative_cooldown_seconds);
    void record_duration_limit(std::uint32_t source_id,
                               std::int64_t now_ms,
                               int cooldown_seconds);
    SourceCooldownInfo check(std::uint32_t source_id, std::int64_t now_ms) const;
    std::vector<DmrSourceCooldown> active(std::int64_t now_ms) const;

private:
    void persist() const;

    std::filesystem::path persistence_path_;
    std::map<std::uint32_t, DmrSourceCooldown> records_;
};

class ConfigurationRebuildInterlock {
public:
    void begin(std::string activation_id);
    void complete(bool success);
    bool inhibited() const;
    const std::string& activation_id() const;

private:
    bool inhibited_ = false;
    std::string activation_id_;
};

class AutomaticTransmitGate {
public:
    explicit AutomaticTransmitGate(TransmitConfig config);

    AutomaticTransmitGrant evaluate(const TxRequest& request,
                                    const RfHealthState& rf_health,
                                    std::int64_t now_ms) const;
    void set_transmit_enabled(bool enabled);
    void set_maintenance_inhibit(bool inhibited);
    void set_rebuild_inhibit(bool inhibited);

private:
    TransmitConfig config_;
    bool maintenance_inhibit_ = false;
    bool rebuild_inhibit_ = false;
};

} // namespace dmr_rpt
