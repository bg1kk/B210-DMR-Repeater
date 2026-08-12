// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "golay2087.h"
#include <cstdint>
#include <vector>

namespace gr {
namespace dmr {
namespace edac {

class Golay24
{
public:
    static int checkAndCorrect(uint32_t& codeword)
    {
        bit_vector bits(20);
        for (int index = 0; index < 20; ++index) {
            bits[index] = ((codeword >> (19 - index)) & 1U) != 0;
        }

        const unsigned int errors = CGolay2087::decode(bits);
        if (errors > 2U) {
            return 2;
        }

        codeword = 0;
        for (bool bit : bits) {
            codeword = (codeword << 1U) | (bit ? 1U : 0U);
        }
        return errors == 0U ? 0 : 1;
    }
};

} // namespace edac
} // namespace dmr
} // namespace gr
