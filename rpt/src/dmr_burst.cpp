// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/dmr_burst.h"

#include <algorithm>
#include <array>

namespace dmr_rpt {
namespace {

struct SyncDefinition {
    std::uint64_t pattern;
    DmrBurstSyncKind kind;
    int direct_slot;
};

constexpr std::array<SyncDefinition, 9> kSyncDefinitions{{
    {0x755FD7DF75F7ULL, DmrBurstSyncKind::Voice, 0},
    {0xDFF57D75DF5DULL, DmrBurstSyncKind::Data, 0},
    {0x7F7D5DD57DFDULL, DmrBurstSyncKind::Voice, 0},
    {0xD5D7F77FD757ULL, DmrBurstSyncKind::Data, 0},
    {0x77D55F7DFD77ULL, DmrBurstSyncKind::Reverse, 0},
    {0x5D577F7757FFULL, DmrBurstSyncKind::Voice, 1},
    {0xF7FDD5DDFD55ULL, DmrBurstSyncKind::Data, 1},
    {0x7DFFD5F55D5FULL, DmrBurstSyncKind::Voice, 2},
    {0xD7557F5FF7F5ULL, DmrBurstSyncKind::Data, 2},
}};

int bit_errors(std::uint64_t value)
{
    int errors = 0;
    while (value != 0U) {
        value &= value - 1U;
        ++errors;
    }
    return errors;
}

} // namespace

DmrBurstSyncObservation inspect_dmr_burst_sync(
    const std::uint8_t* dibits,
    std::size_t count,
    int maximum_bit_errors)
{
    DmrBurstSyncObservation observation;
    if (dibits == nullptr || count != kDmrBurstDibits ||
        maximum_bit_errors < 0) {
        return observation;
    }

    std::uint64_t centre = 0;
    for (std::size_t index = 54U; index < 78U; ++index) {
        centre = (centre << 2U) | (dibits[index] & 0x03U);
    }

    for (const SyncDefinition& definition : kSyncDefinitions) {
        const int errors = bit_errors(centre ^ definition.pattern);
        if (errors < observation.bit_errors) {
            observation.kind = definition.kind;
            observation.bit_errors = errors;
            observation.direct_slot = definition.direct_slot;
            observation.pattern = definition.pattern;
        }
    }
    observation.valid = observation.bit_errors <= maximum_bit_errors;
    if (!observation.valid) {
        observation.kind = DmrBurstSyncKind::Unknown;
        observation.direct_slot = 0;
        observation.pattern = 0;
    }
    return observation;
}

bool relay_start_latency_within_limit(
    std::int64_t detected_at_ms,
    std::int64_t relay_started_at_ms,
    std::int64_t maximum_latency_ms)
{
    return maximum_latency_ms >= 0 &&
        relay_started_at_ms >= detected_at_ms &&
        relay_started_at_ms - detected_at_ms <= maximum_latency_ms;
}

int direct_relay_clock_samples_required(
    int output_dibits,
    int startup_prefill_frames_remaining)
{
    if (output_dibits <= 0 || startup_prefill_frames_remaining > 0) {
        return 0;
    }
    return output_dibits * kDemodSamplesPerDibit;
}

} // namespace dmr_rpt
