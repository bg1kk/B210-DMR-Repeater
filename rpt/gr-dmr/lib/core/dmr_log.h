// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "../dmr_types.h"
#include <iomanip>
#include <iostream>
#include <ostream>
#include <streambuf>

namespace gr {
namespace dmr {

enum class LogCategory { SYNC, FRAME, DECODE, ERROR, STATS };

class NullBuffer : public std::streambuf
{
protected:
    int overflow(int value) override { return value; }
};

class DMRLog
{
public:
    static inline bool enabled = false;

    static std::ostream& log(LogCategory category, float seconds)
    {
        if (!enabled) {
            static NullBuffer buffer;
            static std::ostream stream(&buffer);
            return stream;
        }

        std::cerr << "[gr-dmr " << categoryName(category) << " "
                  << std::fixed << std::setprecision(3) << seconds << "] ";
        return std::cerr;
    }

    static const char* dataTypeShort(uint8_t value)
    {
        switch (static_cast<DataType>(value)) {
        case DataType::PI_HEADER: return "PI";
        case DataType::VOICE_LC_HEADER: return "VLC";
        case DataType::TERMINATOR_LC: return "TLC";
        case DataType::CSBK: return "CSBK";
        case DataType::MBC_HEADER: return "MBC-H";
        case DataType::MBC_CONTINUATION: return "MBC-C";
        case DataType::DATA_HEADER: return "DATA-H";
        case DataType::RATE_1_2_DATA: return "R1/2";
        case DataType::RATE_3_4_DATA: return "R3/4";
        case DataType::IDLE: return "IDLE";
        case DataType::RATE_1_DATA: return "R1";
        case DataType::UNIFIED_SINGLE_BLOCK: return "USB";
        case DataType::EMBEDDED_LC: return "ELC";
        default: return "UNKNOWN";
        }
    }

private:
    static const char* categoryName(LogCategory category)
    {
        switch (category) {
        case LogCategory::SYNC: return "sync";
        case LogCategory::FRAME: return "frame";
        case LogCategory::DECODE: return "decode";
        case LogCategory::ERROR: return "error";
        case LogCategory::STATS: return "stats";
        default: return "log";
        }
    }
};

} // namespace dmr
} // namespace gr
