// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <gnuradio/block.h>
#include <memory>

namespace dmr_b210 {

class direct_framer : virtual public gr::block {
public:
    using sptr = std::shared_ptr<direct_framer>;

    static sptr make(unsigned source_id, unsigned target_id, unsigned color_code,
                     unsigned slot, unsigned voice_bursts,
                     unsigned idle_frames = 0);
};

} // namespace dmr_b210
