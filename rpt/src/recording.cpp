// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#include "dmr_rpt/recording.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace dmr_rpt {

std::string to_string(RecordingMode mode)
{
    switch (mode) {
    case RecordingMode::DmrRelay:
        return "dmr_relay";
    case RecordingMode::DmrDirect:
        return "dmr_direct";
    case RecordingMode::FmRelay:
        return "fm_relay";
    }
    throw std::invalid_argument("unknown recording mode");
}

std::string format_recording_filename(const RecordingMetadata& metadata)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        metadata.started_at);
    std::tm local_time {};
#if defined(_WIN32)
    localtime_s(&local_time, &seconds);
#else
    localtime_r(&seconds, &local_time);
#endif

    std::ostringstream name;
    name << std::put_time(&local_time, "%Y%m%d_%H%M%S")
         << '_' << to_string(metadata.mode)
         << "_src" << metadata.source_id
         << "_dst" << metadata.destination_id
         << "_cc" << metadata.color_code
         << "_ts" << metadata.slot
         << ".mp3";
    return name.str();
}

std::string format_ambe_recording_filename(
    const RecordingMetadata& metadata)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        metadata.started_at);
    std::tm utc_time {};
#if defined(_WIN32)
    gmtime_s(&utc_time, &seconds);
#else
    gmtime_r(&seconds, &utc_time);
#endif

    std::ostringstream name;
    name << std::put_time(&utc_time, "%Y%m%d_%H%M%S")
         << '_' << to_string(metadata.mode)
         << "_src" << metadata.source_id
         << "_dst" << metadata.destination_id
         << "_cc" << metadata.color_code
         << "_ts" << metadata.slot
         << ".ambe";
    return name.str();
}

AmbeBurstFrames extract_ambe_frames(const DmrBurstDibits& burst)
{
    AmbeBurstFrames frames {};
    std::copy_n(burst.begin(), 36U, frames[0].begin());
    std::copy_n(burst.begin() + 36U, 18U, frames[1].begin());
    std::copy_n(burst.begin() + 78U, 18U, frames[1].begin() + 18U);
    std::copy_n(burst.begin() + 96U, 36U, frames[2].begin());
    return frames;
}

} // namespace dmr_rpt
