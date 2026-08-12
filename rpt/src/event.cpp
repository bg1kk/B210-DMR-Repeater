// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/event.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace dmr_rpt {
namespace {

struct RxPowerSample {
    std::chrono::steady_clock::time_point at;
    double power = 0.0;
};

std::mutex g_console_mutex;
std::mutex g_console_output_mutex;
std::deque<RxPowerSample> g_rx_power;

constexpr std::size_t kConsoleLineBytes = 40U;
constexpr auto kRxPowerWindow = std::chrono::milliseconds(200);

char call_type_code(CallType type)
{
    switch (type) {
    case CallType::Private: return 'P';
    case CallType::Group: return 'G';
    case CallType::AllCall: return 'A';
    case CallType::Unknown: break;
    }
    return 'U';
}

std::string short_console_label(const std::string& label)
{
    if (label == "CALL START") return console_token::CallStart;
    if (label == "RELAY START") return console_token::RelayStart;
    if (label == "CALL END") return console_token::CallEnd;
    throw std::invalid_argument("unregistered call console label");
}

std::string short_reason(const std::string& reason)
{
    if (reason == "terminator") return "T";
    if (reason == "inactivity_timeout") return "I";
    if (reason == "duration_limit") return "L";
    return "?";
}

bool registered_failure_stage(const std::string& stage)
{
    return stage == "SYNC" || stage == "SLOT" || stage == "LC" ||
        stage == "DATA" || stage == "CTRL" || stage == "ROUTE" ||
        stage == "SAFE" || stage == "STATE" || stage == "QUEUE" ||
        stage == "FRAME";
}

bool registered_reject_reason(const std::string& reason)
{
    return reason == "none" || reason == "invalid_integrity" ||
        reason == "unsupported_profile" || reason == "source_cooldown" ||
        reason == "configuration_rebuild_inhibit" ||
        reason == "tx_disabled" || reason == "resource_busy" ||
        reason == "remote_unsupported" || reason == "invalid_request" ||
        reason == "analog_fm_suppressed" || reason == "unknown" ||
        reason == "no_sync" || reason == "no_lc";
}

double console_rx_dbfs_locked(std::chrono::steady_clock::time_point now)
{
    while (!g_rx_power.empty() &&
           now - g_rx_power.front().at > kRxPowerWindow) {
        g_rx_power.pop_front();
    }
    if (g_rx_power.empty()) {
        return -999.0;
    }
    double sum = 0.0;
    for (const RxPowerSample& sample : g_rx_power) {
        sum += sample.power;
    }
    return 10.0 * std::log10(std::max(sum / g_rx_power.size(), 1e-20));
}

} // namespace

const char* to_string(DmrProfile profile)
{
    switch (profile) {
    case DmrProfile::DirectLab:
        return "direct_lab";
    case DmrProfile::T1:
        return "t1";
    case DmrProfile::T2:
        return "t2";
    case DmrProfile::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}

const char* to_string(DmrEventKind kind)
{
    switch (kind) {
    case DmrEventKind::Voice:
        return "voice";
    case DmrEventKind::RawData:
        return "raw_data";
    case DmrEventKind::Terminator:
        return "terminator";
    }
    return "unknown";
}

const char* to_string(DmrIntegrity integrity)
{
    return integrity == DmrIntegrity::Valid ? "Valid" : "Invalid";
}

const char* to_string(CallType call_type)
{
    switch (call_type) {
    case CallType::Group:
        return "group";
    case CallType::Private:
        return "private";
    case CallType::AllCall:
        return "all_call";
    case CallType::Unknown:
        return "unknown";
    }
    return "unknown";
}

const char* to_string(TxOrigin origin)
{
    switch (origin) {
    case TxOrigin::DmrForward:
        return "dmr_forward";
    case TxOrigin::RawData:
        return "raw_data";
    case TxOrigin::LocalPtt:
        return "local_ptt";
    case TxOrigin::AnalogFm:
        return "analog_fm";
    }
    return "unknown";
}

const char* to_string(RouteRejectReason reason)
{
    switch (reason) {
    case RouteRejectReason::None:
        return "none";
    case RouteRejectReason::InvalidIntegrity:
        return "invalid_integrity";
    case RouteRejectReason::UnsupportedProfile:
        return "unsupported_profile";
    case RouteRejectReason::SourceCooldown:
        return "source_cooldown";
    case RouteRejectReason::ConfigurationRebuildInhibit:
        return "configuration_rebuild_inhibit";
    case RouteRejectReason::TxDisabled:
        return "tx_disabled";
    case RouteRejectReason::ResourceBusy:
        return "resource_busy";
    case RouteRejectReason::RemoteUnsupported:
        return "remote_unsupported";
    case RouteRejectReason::InvalidRequest:
        return "invalid_request";
    case RouteRejectReason::AnalogFmSuppressed:
        return "analog_fm_suppressed";
    }
    return "unknown";
}

std::vector<std::string> format_call_console_bodies(
    const CallConsoleEvent& event)
{
    std::ostringstream event_line;
    event_line << short_console_label(event.label);
    if (!event.correlation_id.empty()) {
        event_line << ' ' << console_token::CorrelationId
                   << event.correlation_id;
    }

    std::ostringstream identity_line;
    identity_line << console_token::SourceId << '=' << event.source_id
                  << '>' << console_token::DestinationId << '='
                  << event.destination_id;

    std::ostringstream detail_line;
    detail_line << call_type_code(event.call_type)
                << " C" << event.color_code << "T" << event.slot;
    if (event.reason) {
        detail_line << " R" << short_reason(*event.reason);
    }
    if (event.duration_ms) {
        detail_line << " D"
                    << std::max<std::int64_t>(0, *event.duration_ms) / 1000
                    << 's';
    }
    return {event_line.str(), identity_line.str(), detail_line.str()};
}

std::string format_call_console_line(const CallConsoleEvent& event)
{
    const std::vector<std::string> bodies = format_call_console_bodies(event);
    std::ostringstream out;
    for (std::size_t index = 0; index < bodies.size(); ++index) {
        if (index > 0) out << ' ';
        out << bodies[index];
    }
    return out.str();
}

std::vector<std::string> format_signal_reject_console_bodies(
    const SignalRejectConsoleEvent& event)
{
    if (!registered_failure_stage(event.stage)) {
        throw std::invalid_argument("unregistered signal rejection stage");
    }
    if (!registered_reject_reason(event.reason)) {
        throw std::invalid_argument("unregistered signal rejection reason");
    }
    return {
        std::string(console_token::SignalDrop) + " " +
            console_token::FailureStage + "=" + event.stage,
        std::string(console_token::RejectReason) + "=" + event.reason,
    };
}

void update_console_rx_power(double power)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_console_mutex);
    g_rx_power.push_back({now, std::max(power, 0.0)});
    (void)console_rx_dbfs_locked(now);
}

namespace {

std::string format_console_prefix()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif

    std::ostringstream prefix;
    prefix << std::setfill('0') << std::setw(2) << local.tm_hour << ':'
           << std::setw(2) << local.tm_min << ':'
           << std::setw(2) << local.tm_sec << ' '
           << console_token::RxPower << '=';
    {
        std::lock_guard<std::mutex> lock(g_console_mutex);
        const double rx_dbfs = console_rx_dbfs_locked(
            std::chrono::steady_clock::now());
        if (rx_dbfs <= -998.0) {
            prefix << "--.-";
        } else {
            prefix << std::fixed << std::setprecision(1) << rx_dbfs;
        }
    }
    prefix << ' ';
    return prefix.str();
}

void append_wrapped_body(std::vector<std::string>& lines,
                         const std::string& prefix,
                         const std::string& body,
                         bool& continuation)
{
    std::size_t cursor = 0;
    do {
        while (cursor < body.size() && body[cursor] == ' ') ++cursor;
        const std::string marker = continuation ? "+" : "";
        const std::size_t used = prefix.size() + marker.size();
        const std::size_t capacity = used < kConsoleLineBytes
            ? kConsoleLineBytes - used : 0U;
        if (capacity == 0U) {
            lines.push_back((prefix + marker).substr(0, kConsoleLineBytes));
            return;
        }

        const std::size_t remaining = body.size() - cursor;
        std::size_t take = std::min(capacity, remaining);
        if (take < remaining) {
            const std::size_t break_at = body.rfind(' ', cursor + take - 1U);
            if (break_at != std::string::npos && break_at > cursor) {
                take = break_at - cursor;
            }
        }
        lines.push_back(prefix + marker + body.substr(cursor, take));
        cursor += take;
        while (cursor < body.size() && body[cursor] == ' ') ++cursor;
        continuation = true;
    } while (cursor < body.size());
}

} // namespace

std::vector<std::string> format_console_lines(
    const std::vector<std::string>& bodies)
{
    const std::string prefix = format_console_prefix();
    std::vector<std::string> lines;
    bool continuation = false;
    if (bodies.empty()) {
        lines.push_back(prefix);
        return lines;
    }
    for (const std::string& body : bodies) {
        append_wrapped_body(lines, prefix, body, continuation);
    }
    return lines;
}

std::vector<std::string> format_console_lines(const std::string& body)
{
    return format_console_lines(std::vector<std::string>{body});
}

void write_console_message(std::ostream& output,
                           const std::vector<std::string>& bodies)
{
    const std::vector<std::string> lines = format_console_lines(bodies);
    std::lock_guard<std::mutex> lock(g_console_output_mutex);
    for (const std::string& line : lines) {
        output << line << std::endl;
    }
}

void write_console_message(std::ostream& output, const std::string& body)
{
    write_console_message(output, std::vector<std::string>{body});
}

std::string format_duration_hms(std::int64_t duration_ms)
{
    const std::int64_t total_seconds =
        std::max<std::int64_t>(0, duration_ms) / 1000;
    const std::int64_t hours = total_seconds / 3600;
    const std::int64_t minutes = (total_seconds / 60) % 60;
    const std::int64_t seconds = total_seconds % 60;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setw(2) << minutes << ':' << std::setw(2) << seconds;
    return out.str();
}

bool call_inactivity_expired(std::int64_t last_valid_burst_ms,
                             std::int64_t now_ms,
                             int timeout_ms)
{
    return timeout_ms >= 0 && now_ms >= last_valid_burst_ms &&
        now_ms - last_valid_burst_ms >= timeout_ms;
}

} // namespace dmr_rpt
