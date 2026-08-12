// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include "dmr_rpt/config.h"
#include "dmr_rpt/event.h"

namespace dmr_rpt {

using SourceCooldownView = std::function<SourceCooldownInfo(std::uint32_t source_id)>;

class CallRouter {
public:
    CallRouter(RoutingConfig routing,
               TransmitConfig transmit,
               SourceCooldownView cooldown_view);

    RouteDecision route_dmr_event(const DmrEvent& event) const;
    RouteDecision route_local_ptt(const TxRequest& request) const;
    RouteDecision route_analog_fm(const TxRequest& request,
                                  bool ctcss_qualified,
                                  bool dmr_idle) const;
    RouteDecision route_remote_request(const TxRequest& request) const;

    void set_configuration_rebuild_inhibit(bool inhibited);

private:
    RouteDecision reject(RouteRejectReason reason, const char* rule) const;
    RouteDecision accept(const TxRequest& request, const char* rule) const;

    RoutingConfig routing_;
    TransmitConfig transmit_;
    SourceCooldownView cooldown_view_;
    bool configuration_rebuild_inhibit_ = false;
};

} // namespace dmr_rpt
