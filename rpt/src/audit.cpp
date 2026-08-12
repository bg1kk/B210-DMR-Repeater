// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/audit.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

namespace dmr_rpt {

OperationAuditLogger::OperationAuditLogger(LoggingConfig config,
                                           UtcProvider utc_provider)
    : config_(std::move(config))
    , utc_provider_(std::move(utc_provider))
{
    open_sink();
}

OperationAuditLogger::~OperationAuditLogger()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr) {
        stream_->flush();
        delete stream_;
        stream_ = nullptr;
    }
}

void OperationAuditLogger::emit(const AuditEvent& event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++health_.queued_events;
    if (stream_ == nullptr || !*stream_) {
        health_.sink_healthy = false;
        ++health_.dropped_events;
        health_.last_error = "audit sink unavailable";
        return;
    }

    const std::string timestamp = json_escape(now_utc());
    *stream_ << '{'
             << "\"Timestamp\":\"" << timestamp << "\","
             << "\"event_time_utc\":\"" << timestamp << "\","
             << "\"event_seq\":" << next_seq_++ << ','
             << "\"component\":\"" << json_escape(event.component) << "\","
             << "\"event_type\":\"" << json_escape(event.event_type) << "\","
             << "\"operation\":\"" << json_escape(event.operation) << "\","
             << "\"result\":\"" << json_escape(event.result) << "\","
             << "\"correlation_id\":\"" << json_escape(event.correlation_id) << "\"";
    for (const auto& field : event.fields) {
        if (!field_allowed(field.first)) {
            continue;
        }
        *stream_ << ",\"" << json_escape(field.first) << "\":\""
                 << json_escape(field.second) << "\"";
    }
    *stream_ << "}\n";
    stream_->flush();
    if (!*stream_) {
        health_.sink_healthy = false;
        ++health_.dropped_events;
        health_.last_error = "audit write or flush failed";
        return;
    }
    ++health_.written_events;
}

const AuditHealth& OperationAuditLogger::health() const
{
    return health_;
}

const std::filesystem::path& OperationAuditLogger::path() const
{
    return path_;
}

std::string OperationAuditLogger::now_utc() const
{
    if (utc_provider_) {
        return utc_provider_();
    }
    return default_utc_now();
}

std::string OperationAuditLogger::default_utc_now()
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string OperationAuditLogger::json_escape(const std::string& value)
{
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

bool OperationAuditLogger::field_allowed(const std::string& key)
{
    static const std::set<std::string> kBlocked{
        "payload", "raw_payload", "dibits", "iq", "pcm", "ambe",
        "hmac", "auth_tag", "nonce", "key", "secret", "key_file",
        "config_text", "patch_value"
    };
    return kBlocked.find(key) == kBlocked.end();
}

void OperationAuditLogger::open_sink()
{
    if (config_.event_directory.empty()) {
        config_.event_directory = ".";
    }
    std::filesystem::create_directories(config_.event_directory);
    const std::string stamp = now_utc();
    std::string compact;
    compact.reserve(stamp.size());
    for (char ch : stamp) {
        if (ch != '-' && ch != ':') {
            compact.push_back(ch);
        }
    }
    if (!compact.empty() && compact.back() == 'Z') {
        compact.pop_back();
    }
    const std::string base = config_.startup_file_prefix + "_" + compact + "Z";
    for (int suffix = 0; suffix < 1000; ++suffix) {
        std::ostringstream name;
        name << base;
        if (suffix > 0) {
            name << '_' << std::setw(3) << std::setfill('0') << suffix;
        }
        name << ".jsonl";
        const std::filesystem::path candidate = config_.event_directory / name.str();
        if (std::filesystem::exists(candidate)) {
            continue;
        }
        std::ofstream* stream = new std::ofstream(candidate, std::ios::out | std::ios::trunc);
        if (!*stream) {
            delete stream;
            throw ConfigError("cannot create audit file: " + candidate.string());
        }
        stream_ = stream;
        path_ = candidate;
        return;
    }
    throw ConfigError("cannot allocate unique audit file in " +
                      config_.event_directory.string());
}

} // namespace dmr_rpt
