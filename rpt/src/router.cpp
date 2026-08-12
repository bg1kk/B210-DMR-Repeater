// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/router.h"

#include <utility>

namespace dmr_rpt {

CallRouter::CallRouter(RoutingConfig routing,
                       TransmitConfig transmit,
                       SourceCooldownView cooldown_view)
    : routing_(std::move(routing))
    , transmit_(transmit)
    , cooldown_view_(std::move(cooldown_view))
{
}

RouteDecision CallRouter::route_dmr_event(const DmrEvent& event) const
{
    if (configuration_rebuild_inhibit_) {
        return reject(RouteRejectReason::ConfigurationRebuildInhibit,
                      "configuration_rebuild_inhibit");
    }
    if (!transmit_.enabled) {
        return reject(RouteRejectReason::TxDisabled, "SAFE transmit.enabled");
    }
    if (routing_.policy != "route_all_valid_etsi") {
        return reject(RouteRejectReason::InvalidRequest, "routing.policy");
    }
    if (event.integrity != DmrIntegrity::Valid) {
        return reject(RouteRejectReason::InvalidIntegrity,
                      "ETSI integrity gate");
    }
    if (!event.supported_etsi_service || event.profile == DmrProfile::Unsupported) {
        return reject(RouteRejectReason::UnsupportedProfile,
                      "AIR profile enablement");
    }
    if (event.kind == DmrEventKind::RawData) {
        return reject(RouteRejectReason::UnsupportedProfile,
                      "DATA service disabled");
    }

    TxRequest request;
    request.profile = event.profile;
    request.slot = event.slot;
    request.color_code = event.color_code;
    request.source_id = event.source_id;
    request.destination_id = event.destination_id;
    request.call_type = event.call_type;
    request.correlation_id = event.correlation_id;
    request.origin = TxOrigin::DmrForward;

    if (request.origin == TxOrigin::DmrForward && request.source_id && cooldown_view_) {
        const SourceCooldownInfo cooldown = cooldown_view_(*request.source_id);
        if (cooldown.active) {
            RouteDecision decision = reject(RouteRejectReason::SourceCooldown,
                                            "SAFE DmrSourceCooldownView");
            decision.actions.push_back("Drop");
            return decision;
        }
    }

    return accept(request, "route_all_valid_etsi");
}

RouteDecision CallRouter::route_local_ptt(const TxRequest& request) const
{
    if (configuration_rebuild_inhibit_) {
        return reject(RouteRejectReason::ConfigurationRebuildInhibit,
                      "configuration_rebuild_inhibit");
    }
    if (!transmit_.enabled) {
        return reject(RouteRejectReason::TxDisabled, "SAFE transmit.enabled");
    }
    if (request.origin != TxOrigin::LocalPtt ||
        request.call_type != CallType::AllCall ||
        !request.destination_id ||
        *request.destination_id != 0xFFFFFFU) {
        return reject(RouteRejectReason::InvalidRequest,
                      "AUDIO LocalPttTxContext");
    }
    return accept(request, "local_ptt_tx_context");
}

RouteDecision CallRouter::route_analog_fm(const TxRequest& request,
                                          bool ctcss_qualified,
                                          bool dmr_idle) const
{
    if (configuration_rebuild_inhibit_) {
        return reject(RouteRejectReason::ConfigurationRebuildInhibit,
                      "configuration_rebuild_inhibit");
    }
    if (!ctcss_qualified || !dmr_idle) {
        return reject(RouteRejectReason::AnalogFmSuppressed,
                      "AFM CTCSS and DMR idle gate");
    }
    if (request.origin != TxOrigin::AnalogFm ||
        !request.source_id || *request.source_id != 9999U ||
        !request.destination_id || *request.destination_id != 0xFFFFFFU ||
        request.call_type != CallType::AllCall) {
        return reject(RouteRejectReason::InvalidRequest,
                      "AnalogFmTxRequest fixed all-call context");
    }
    return accept(request, "analog_fm_fallback");
}

RouteDecision CallRouter::route_remote_request(const TxRequest&) const
{
    return reject(RouteRejectReason::RemoteUnsupported,
                  "NET reserved no-I/O boundary");
}

void CallRouter::set_configuration_rebuild_inhibit(bool inhibited)
{
    configuration_rebuild_inhibit_ = inhibited;
}

RouteDecision CallRouter::reject(RouteRejectReason reason, const char* rule) const
{
    RouteDecision decision;
    decision.accepted = false;
    decision.reason = reason;
    decision.standard_rule = rule;
    decision.actions.push_back("Drop");
    return decision;
}

RouteDecision CallRouter::accept(const TxRequest& request, const char* rule) const
{
    RouteDecision decision;
    decision.accepted = true;
    decision.reason = RouteRejectReason::None;
    decision.standard_rule = rule;
    decision.actions.push_back("RF");
    if (request.origin == TxOrigin::DmrForward) {
        decision.actions.push_back("LocalMonitor");
    }
    decision.tx_request = request;
    return decision;
}

} // namespace dmr_rpt
