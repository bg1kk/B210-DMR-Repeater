// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <gnuradio/dmr/api.h>
#include <gnuradio/sync_block.h>
#include <cstdint>
#include <memory>

namespace gr {
namespace dmr {

class DMR_API frame_decoder : virtual public gr::sync_block
{
public:
    using sptr = std::shared_ptr<frame_decoder>;

    static sptr make(float sample_rate = 4800.0f,
                     int slot = 0,
                     int color_code = -1,
                     bool test_mode = false);

    virtual void set_slot(int slot) = 0;
    virtual void set_color_code(int color_code) = 0;
    virtual uint64_t get_sync_count() const = 0;
    virtual uint64_t get_frame_count() const = 0;
    virtual void reset() = 0;
    virtual void set_debug(bool enable) = 0;
};

} // namespace dmr
} // namespace gr
