// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace gr {
namespace dmr {

struct dmr_lc_data {
    bool valid = false;
    uint8_t pf = 0;
    uint8_t flco = 0;
    uint8_t fid = 0;
    uint8_t svcopt = 0;
    uint32_t dstaddr = 0;
    uint32_t srcaddr = 0;
};

class dmr_slot
{
public:
    using lc_callback =
        std::function<void(int, uint8_t, const dmr_lc_data&, bool)>;

    dmr_slot(int slot, int debug);

    void set_lc_callback(lc_callback callback);
    void set_cc(uint8_t color_code);
    uint8_t get_cc() const;
    bool load_slot(const uint8_t slot_bits[264], uint64_t sync_type);

private:
    bool decode_embedded_lc();

    int d_slot;
    int d_debug;
    int d_color_code;
    std::vector<bool> d_fragments;
    lc_callback d_callback;
};

} // namespace dmr
} // namespace gr
