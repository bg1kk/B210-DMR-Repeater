// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dmr_rpt {

constexpr std::size_t kDmrBurstDibits = 132U;
constexpr int kDmrReliableSyncErrorLimit = 6;
constexpr std::int64_t kMaximumRelayStartLatencyMs = 500;
constexpr int kDirectModeFrameDibits = 288;
constexpr int kDemodSamplesPerDibit = 10;
// Keep enough zero-gated samples ahead of the USRP while GNU Radio starts.
// Two 60 ms frames provide the fixed 120 ms single-stream startup reserve.
constexpr int kSingleStreamTxStartupPrefillFrames = 2;
constexpr int kDirectRelayTxOutputBufferFrames = 8;
constexpr int kDirectModeFrameDurationMs = 60;

using RawDmrBurst = std::array<std::uint8_t, kDmrBurstDibits>;

enum class DmrBurstSyncKind {
    Unknown,
    Data,
    Voice,
    Reverse,
};

struct DmrBurstSyncObservation {
    bool valid = false;
    DmrBurstSyncKind kind = DmrBurstSyncKind::Unknown;
    int bit_errors = 49;
    int direct_slot = 0;
    std::uint64_t pattern = 0;
};

DmrBurstSyncObservation inspect_dmr_burst_sync(
    const std::uint8_t* dibits,
    std::size_t count,
    int maximum_bit_errors = kDmrReliableSyncErrorLimit);

inline DmrBurstSyncObservation inspect_dmr_burst_sync(
    const RawDmrBurst& burst,
    int maximum_bit_errors = kDmrReliableSyncErrorLimit)
{
    return inspect_dmr_burst_sync(
        burst.data(), burst.size(), maximum_bit_errors);
}

bool relay_start_latency_within_limit(
    std::int64_t detected_at_ms,
    std::int64_t relay_started_at_ms,
    std::int64_t maximum_latency_ms = kMaximumRelayStartLatencyMs);

int direct_relay_clock_samples_required(
    int output_dibits,
    int startup_prefill_frames_remaining);

} // namespace dmr_rpt
