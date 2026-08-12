// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include "dmr_slot.h"
#include "dmr_types.h"
#include <gnuradio/dmr/frame_decoder.h>
#include <pmt/pmt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

namespace gr {
namespace dmr {

class MovingAverage
{
public:
    explicit MovingAverage(std::size_t size)
        : d_values(size, 0.0f)
    {
    }

    void add(float value)
    {
        d_sum -= d_values[d_index];
        d_values[d_index] = value;
        d_sum += value;
        d_index = (d_index + 1) % d_values.size();
        if (d_count < d_values.size()) {
            ++d_count;
        }
    }

    float avg() const
    {
        return d_count == 0 ? 0.0f : d_sum / static_cast<float>(d_count);
    }

    void reset()
    {
        std::fill(d_values.begin(), d_values.end(), 0.0f);
        d_index = 0;
        d_count = 0;
        d_sum = 0.0f;
    }

private:
    std::vector<float> d_values;
    std::size_t d_index = 0;
    std::size_t d_count = 0;
    float d_sum = 0.0f;
};

class frame_decoder_impl : public frame_decoder
{
public:
    frame_decoder_impl(float sample_rate,
                       int slot,
                       int color_code,
                       bool test_mode);
    ~frame_decoder_impl() override;

    void set_slot(int slot) override;
    void set_color_code(int color_code) override;
    uint64_t get_sync_count() const override;
    uint64_t get_frame_count() const override;
    void reset() override;
    void set_debug(bool enable) override;

    int work(int noutput_items,
             gr_vector_const_void_star& input_items,
             gr_vector_void_star& output_items) override;

private:
    enum class State { SEARCHING, COLLECTING, DECODING };
    enum class VoiceState { IDLE, IN_SUPERFRAME };

    static int popcount64(uint64_t value);
    SyncPattern match_sync(uint64_t pattern, int& errors);
    int find_best_sync_error();
    Slot determine_slot(SyncPattern sync);
    uint8_t quantize_symbol(float sample);
    void add_dibit(uint8_t dibit);
    void decode_frame();
    bool decode_slot_type(DMRBurst& burst);
    bool decode_payload(DMRBurst& burst);
    bool decode_trellis_data(DMRBurst& burst);
    pmt::pmt_t burst_to_pmt(const DMRBurst& burst);
    void publish_burst(const DMRBurst& burst);
    void process_sample(float sample);
    void test_thread_func();
    void generate_test_frame();
    void enter_voice_superframe(uint64_t sync_type);
    void check_voice_frame_timing();
    void collect_voice_frame();
    void decode_embedded_signaling();
    void on_embedded_lc_complete(const dmr_lc_data& lc);

    float d_sample_rate;
    int d_slot_filter;
    int d_color_code_filter;
    bool d_test_mode;
    float d_omega;
    float d_omega_mid;
    float d_omega_rel;
    float d_mu;
    float d_gain_mu;
    float d_gain_omega;
    float d_last_sample;
    float d_min_omega;
    float d_max_omega;
    float* d_dl;
    int d_dl_index;
    int d_twice_sps;
    MovingAverage d_lock_accum;
    float d_lock_threshold;
    float d_level_min;
    float d_level_max;
    float d_level_alpha;
    float d_level_center;
    uint64_t d_sync_register;
    bool d_sync_found;
    SyncPattern d_current_sync;
    int d_sync_errors;
    State d_state;
    int d_bits_collected;
    std::array<uint8_t, 264> d_frame_bits;
    std::deque<uint8_t> d_dibit_buffer;
    std::atomic<uint64_t> d_sync_count;
    std::atomic<uint64_t> d_frame_count;
    uint64_t d_total_samples;
    uint64_t d_samples_since_sync;
    Slot d_current_slot;
    uint64_t d_last_frame_symbols;
    VoiceState d_voice_state;
    uint64_t d_voice_sync_type;
    int d_voice_frame_index;
    uint64_t d_next_frame_symbols;
    uint64_t d_symbol_count;
    dmr_slot d_dmr_slot;
    pmt::pmt_t d_port_out;
    std::atomic<bool> d_test_running;
    std::thread d_test_thread;
};

} // namespace dmr
} // namespace gr
