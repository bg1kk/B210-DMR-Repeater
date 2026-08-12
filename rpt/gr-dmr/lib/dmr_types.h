// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace gr {
namespace dmr {

constexpr float DMR_SYMBOL_RATE = 4800.0f;
constexpr int NUM_FLOAT = 1024;
constexpr std::size_t DIBIT_BUFFER_SIZE = 4096;
constexpr uint64_t FRAME_SYMBOLS = 144;
constexpr uint64_t FRAME_SYMBOL_TOLERANCE = 8;
constexpr int SUPERFRAME_FRAMES = 6;
constexpr float SILENCE_RESET_SECONDS = 2.0f;

constexpr uint64_t SYNC_BS_VOICE = 0x755fd7df75f7ULL;
constexpr uint64_t SYNC_BS_DATA = 0xdff57d75df5dULL;
constexpr uint64_t SYNC_MS_VOICE = 0x7f7d5dd57dfdULL;
constexpr uint64_t SYNC_MS_DATA = 0xd5d7f77fd757ULL;
constexpr uint64_t SYNC_MS_REVERSE = 0x77d55f7dfd77ULL;
constexpr uint64_t SYNC_DIRECT_VOICE_TS1 = 0x5d577f7757ffULL;
constexpr uint64_t SYNC_DIRECT_DATA_TS1 = 0xf7fdd5ddfd55ULL;
constexpr uint64_t SYNC_DIRECT_VOICE_TS2 = 0x7dffd5f55d5fULL;
constexpr uint64_t SYNC_DIRECT_DATA_TS2 = 0xd7557f5ff7f5ULL;

constexpr std::array<uint64_t, 9> SYNC_PATTERNS{
    SYNC_BS_VOICE,
    SYNC_BS_DATA,
    SYNC_MS_VOICE,
    SYNC_MS_DATA,
    SYNC_MS_REVERSE,
    SYNC_DIRECT_VOICE_TS1,
    SYNC_DIRECT_DATA_TS1,
    SYNC_DIRECT_VOICE_TS2,
    SYNC_DIRECT_DATA_TS2,
};
constexpr int NUM_SYNC_PATTERNS = static_cast<int>(SYNC_PATTERNS.size());

constexpr std::array<int, 20> SLOT_TYPE_INDICES_264{
    98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
    156, 157, 158, 159, 160, 161, 162, 163, 164, 165,
};

enum class SyncPattern : uint64_t {
    UNKNOWN = 0,
    BS_VOICE_SYNC = SYNC_BS_VOICE,
    BS_DATA_SYNC = SYNC_BS_DATA,
    MS_VOICE_SYNC = SYNC_MS_VOICE,
    MS_DATA_SYNC = SYNC_MS_DATA,
    MS_REVERSE_SYNC = SYNC_MS_REVERSE,
    DIRECT_VOICE_TS1 = SYNC_DIRECT_VOICE_TS1,
    DIRECT_DATA_TS1 = SYNC_DIRECT_DATA_TS1,
    DIRECT_VOICE_TS2 = SYNC_DIRECT_VOICE_TS2,
    DIRECT_DATA_TS2 = SYNC_DIRECT_DATA_TS2,
};

enum class Slot : int { UNKNOWN = 0, SLOT1 = 1, SLOT2 = 2 };

enum class DataType : uint8_t {
    PI_HEADER = 0x0,
    VOICE_LC_HEADER = 0x1,
    TERMINATOR_LC = 0x2,
    CSBK = 0x3,
    MBC_HEADER = 0x4,
    MBC_CONTINUATION = 0x5,
    DATA_HEADER = 0x6,
    RATE_1_2_DATA = 0x7,
    RATE_3_4_DATA = 0x8,
    IDLE = 0x9,
    RATE_1_DATA = 0xA,
    UNIFIED_SINGLE_BLOCK = 0xB,
    EMBEDDED_LC = 0x10,
    UNKNOWN = 0xFF,
};

enum class CallType : int { GROUP_CALL = 0, PRIVATE_CALL = 1 };

struct DMRBurst {
    uint64_t sample_index = 0;
    uint64_t timestamp_us = 0;
    SyncPattern sync_pattern = SyncPattern::UNKNOWN;
    int sync_errors = 0;
    bool sync_valid = false;
    Slot slot = Slot::UNKNOWN;
    uint8_t color_code = 0;
    DataType data_type = DataType::UNKNOWN;
    bool slot_type_valid = false;
    std::array<uint8_t, 33> raw_bytes{};
    std::array<uint8_t, 12> payload{};
    bool data_valid = false;
    bool fec_valid = false;
    uint32_t source_id = 0;
    uint32_t dest_id = 0;
    CallType call_type = CallType::GROUP_CALL;
    bool emergency = false;
    bool lc_valid = false;
    uint8_t csbk_opcode = 0;
    bool csbk_last_block = false;
};

inline bool isVoiceSync(SyncPattern pattern)
{
    return pattern == SyncPattern::BS_VOICE_SYNC ||
           pattern == SyncPattern::MS_VOICE_SYNC ||
           pattern == SyncPattern::DIRECT_VOICE_TS1 ||
           pattern == SyncPattern::DIRECT_VOICE_TS2;
}

inline bool isDataSync(SyncPattern pattern)
{
    return pattern == SyncPattern::BS_DATA_SYNC ||
           pattern == SyncPattern::MS_DATA_SYNC ||
           pattern == SyncPattern::DIRECT_DATA_TS1 ||
           pattern == SyncPattern::DIRECT_DATA_TS2;
}

inline uint64_t syncPatternToMagic(SyncPattern pattern)
{
    return static_cast<uint64_t>(pattern);
}

inline const char* syncPatternToString(SyncPattern pattern)
{
    switch (pattern) {
    case SyncPattern::BS_VOICE_SYNC: return "BS_VOICE";
    case SyncPattern::BS_DATA_SYNC: return "BS_DATA";
    case SyncPattern::MS_VOICE_SYNC: return "MS_VOICE";
    case SyncPattern::MS_DATA_SYNC: return "MS_DATA";
    case SyncPattern::MS_REVERSE_SYNC: return "MS_REVERSE";
    case SyncPattern::DIRECT_VOICE_TS1: return "DIRECT_VOICE_TS1";
    case SyncPattern::DIRECT_DATA_TS1: return "DIRECT_DATA_TS1";
    case SyncPattern::DIRECT_VOICE_TS2: return "DIRECT_VOICE_TS2";
    case SyncPattern::DIRECT_DATA_TS2: return "DIRECT_DATA_TS2";
    default: return "UNKNOWN";
    }
}

inline std::string dataTypeToString(DataType type)
{
    switch (type) {
    case DataType::PI_HEADER: return "PI_HEADER";
    case DataType::VOICE_LC_HEADER: return "VOICE_LC_HEADER";
    case DataType::TERMINATOR_LC: return "TERMINATOR_LC";
    case DataType::CSBK: return "CSBK";
    case DataType::MBC_HEADER: return "MBC_HEADER";
    case DataType::MBC_CONTINUATION: return "MBC_CONTINUATION";
    case DataType::DATA_HEADER: return "DATA_HEADER";
    case DataType::RATE_1_2_DATA: return "RATE_1_2_DATA";
    case DataType::RATE_3_4_DATA: return "RATE_3_4_DATA";
    case DataType::IDLE: return "IDLE";
    case DataType::RATE_1_DATA: return "RATE_1_DATA";
    case DataType::UNIFIED_SINGLE_BLOCK: return "UNIFIED_SINGLE_BLOCK";
    case DataType::EMBEDDED_LC: return "EMBEDDED_LC";
    default: return "UNKNOWN";
    }
}

} // namespace dmr
} // namespace gr
