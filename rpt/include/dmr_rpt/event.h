// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace dmr_rpt {

namespace console_token {

inline constexpr char RxPower[] = "Rssi";
inline constexpr char Ctcss[] = "CT";
inline constexpr char DisableFm[] = "disFM";
inline constexpr char SquelchOpen[] = "SQL_on";
inline constexpr char SquelchClosed[] = "SQL_off";
inline constexpr char CallStart[] = "Call Start";
inline constexpr char RelayStart[] = "Rpt Start";
inline constexpr char CallEnd[] = "Call End";
inline constexpr char SourceId[] = "SID";
inline constexpr char DestinationId[] = "DID";
inline constexpr char CorrelationId[] = "Q";
inline constexpr char RecordingStart[] = "Recorder Start";
inline constexpr char RecordingStop[] = "Recorder Stop";
inline constexpr char RecordingFail[] = "Recorder Fail";
inline constexpr char SignalDrop[] = "Drop";
inline constexpr char FailureStage[] = "S";
inline constexpr char RejectReason[] = "R";

} // namespace console_token

enum class DmrProfile {
    DirectLab,
    T1,
    T2,
    Unsupported,
};

enum class DmrEventKind {
    Voice,
    RawData,
    Terminator,
};

enum class DmrIntegrity {
    Valid,
    Invalid,
};

enum class CallType {
    Group,
    Private,
    AllCall,
    Unknown,
};

enum class TxOrigin {
    DmrForward,
    RawData,
    LocalPtt,
    AnalogFm,
};

enum class RouteRejectReason {
    None,
    InvalidIntegrity,
    UnsupportedProfile,
    SourceCooldown,
    ConfigurationRebuildInhibit,
    TxDisabled,
    ResourceBusy,
    RemoteUnsupported,
    InvalidRequest,
    AnalogFmSuppressed,
};

struct RawDataBurst {
    std::vector<std::uint8_t> dibits;
    std::vector<std::uint8_t> raw_payload;
};

struct DmrEvent {
    DmrEventKind kind = DmrEventKind::Voice;
    DmrProfile profile = DmrProfile::T2;
    DmrIntegrity integrity = DmrIntegrity::Invalid;
    int slot = 1;
    int color_code = 1;
    std::optional<std::uint32_t> source_id;
    std::optional<std::uint32_t> destination_id;
    CallType call_type = CallType::Unknown;
    bool supported_etsi_service = true;
    RawDataBurst raw_data;
    std::string correlation_id;
};

struct TxRequest {
    TxOrigin origin = TxOrigin::DmrForward;
    DmrProfile profile = DmrProfile::T2;
    int slot = 1;
    int color_code = 1;
    std::optional<std::uint32_t> source_id;
    std::optional<std::uint32_t> destination_id;
    CallType call_type = CallType::Unknown;
    std::string correlation_id;
};

struct SourceCooldownInfo {
    bool active = false;
    std::int64_t remaining_ms = 0;
};

struct RouteDecision {
    bool accepted = false;
    RouteRejectReason reason = RouteRejectReason::None;
    std::string standard_rule;
    std::vector<std::string> actions;
    std::optional<TxRequest> tx_request;
};

struct CallConsoleEvent {
    std::string label;
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
    CallType call_type = CallType::Unknown;
    int color_code = 0;
    int slot = 0;
    std::string correlation_id;
    std::optional<std::string> reason;
    std::optional<std::int64_t> duration_ms;
};

struct SignalRejectConsoleEvent {
    std::string stage;
    std::string reason;
};

const char* to_string(DmrProfile profile);
const char* to_string(DmrEventKind kind);
const char* to_string(DmrIntegrity integrity);
const char* to_string(CallType call_type);
const char* to_string(TxOrigin origin);
const char* to_string(RouteRejectReason reason);
std::vector<std::string> format_call_console_bodies(
    const CallConsoleEvent& event);
std::string format_call_console_line(const CallConsoleEvent& event);
std::vector<std::string> format_signal_reject_console_bodies(
    const SignalRejectConsoleEvent& event);
std::string format_duration_hms(std::int64_t duration_ms);
void update_console_rx_power(double power);
std::vector<std::string> format_console_lines(const std::string& body);
std::vector<std::string> format_console_lines(
    const std::vector<std::string>& bodies);
void write_console_message(std::ostream& output, const std::string& body);
void write_console_message(std::ostream& output,
                           const std::vector<std::string>& bodies);
bool call_inactivity_expired(std::int64_t last_valid_burst_ms,
                             std::int64_t now_ms,
                             int timeout_ms);

} // namespace dmr_rpt
