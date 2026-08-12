// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "dmr_rpt/config.h"

namespace dmr_rpt {

struct AuditEvent {
    std::string component;
    std::string event_type;
    std::string operation;
    std::string result;
    std::string correlation_id;
    std::map<std::string, std::string> fields;
};

struct AuditHealth {
    bool sink_healthy = true;
    std::uint64_t queued_events = 0;
    std::uint64_t written_events = 0;
    std::uint64_t dropped_events = 0;
    std::string last_error;
};

class OperationAuditLogger {
public:
    using UtcProvider = std::function<std::string()>;

    OperationAuditLogger(LoggingConfig config, UtcProvider utc_provider = {});
    ~OperationAuditLogger();

    OperationAuditLogger(const OperationAuditLogger&) = delete;
    OperationAuditLogger& operator=(const OperationAuditLogger&) = delete;

    void emit(const AuditEvent& event);
    const AuditHealth& health() const;
    const std::filesystem::path& path() const;

private:
    std::string now_utc() const;
    static std::string default_utc_now();
    static std::string json_escape(const std::string& value);
    static bool field_allowed(const std::string& key);
    void open_sink();

    LoggingConfig config_;
    UtcProvider utc_provider_;
    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::uint64_t next_seq_ = 1;
    AuditHealth health_;
    std::ofstream* stream_ = nullptr;
};

} // namespace dmr_rpt
