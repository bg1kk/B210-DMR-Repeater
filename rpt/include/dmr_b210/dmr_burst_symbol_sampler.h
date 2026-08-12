// Compiled and written by BG1KK.
// Privatization and closed-source use are strictly forbidden.
// GNU Radio components are copyrighted by their respective developers.
// All other code copyright © BG1KK.
// This copyright statement must be retained.
#pragma once

#include <gnuradio/block.h>
#include <gnuradio/io_signature.h>
#include <pmt/pmt.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

namespace dmr_b210 {

class SharedDmrBurstSymbolSampler final : public gr::block {
public:
    using sptr = std::shared_ptr<SharedDmrBurstSymbolSampler>;
    using LogCallback = std::function<void(const std::string&)>;

    static sptr make(bool verbose, LogCallback logger = {})
    {
        return std::make_shared<SharedDmrBurstSymbolSampler>(
            verbose, std::move(logger));
    }

    explicit SharedDmrBurstSymbolSampler(bool verbose, LogCallback logger)
        : gr::block("dmr_burst_symbol_sampler",
                    gr::io_signature::make(1, 1, sizeof(float)),
                    gr::io_signature::make(1, 1, sizeof(float)))
        , verbose_(verbose), logger_(std::move(logger))
    {
        message_port_register_out(pmt::mp("bursts"));
    }

    void forecast(int, gr_vector_int& required) override
    {
        required[0] = 1;
    }

    int general_work(int noutput_items,
                     gr_vector_int& ninput_items,
                     gr_vector_const_void_star& input_items,
                     gr_vector_void_star& output_items) override
    {
        const auto* input = static_cast<const float*>(input_items[0]);
        auto* output = static_cast<float*>(output_items[0]);
        int produced = drain_pending(output, noutput_items);
        int consumed = 0;

        while (consumed < ninput_items[0] && pending_index_ >= pending_.size()) {
            const float sample = input[consumed++];
            const uint64_t sample_index = absolute_sample_index_++;
            if (scheduled_) {
                if (sample_index >= scheduled_window_start_ &&
                    sample_index < scheduled_window_end_) {
                    scheduled_samples_.push_back(sample);
                    if (sample_index + 1U == scheduled_window_end_) {
                        process_scheduled_burst();
                    }
                }
                continue;
            }
            const bool quiet = std::abs(sample) < 0.05f;
            if (!in_burst_) {
                if (!quiet) {
                    in_burst_ = true;
                    burst_start_absolute_ = sample_index - pre_roll_.size();
                    quiet_run_ = 0;
                    burst_.assign(pre_roll_.begin(), pre_roll_.end());
                    burst_.push_back(sample);
                    pre_roll_.clear();
                } else {
                    pre_roll_.push_back(sample);
                    if (pre_roll_.size() > 40U) {
                        pre_roll_.pop_front();
                    }
                }
                continue;
            }

            burst_.push_back(sample);
            quiet_run_ = quiet ? quiet_run_ + 1 : 0;
            if (quiet_run_ >= 40) {
                pre_roll_.assign(burst_.end() - static_cast<std::ptrdiff_t>(quiet_run_),
                                 burst_.end());
                burst_.resize(burst_.size() - quiet_run_);
                select_symbols();
                burst_.clear();
                quiet_run_ = 0;
                in_burst_ = false;
            } else if (burst_.size() > 3000) {
                // Strong direct-mode signals may not expose a quiet guard interval.
                const bool acquired = select_symbols(true);
                if (verbose_ && !acquired) {
                    log("SB R>3000");
                }
                burst_.clear();
                pre_roll_.clear();
                quiet_run_ = 0;
                in_burst_ = false;
            }
        }
        consume_each(consumed);
        if (produced < noutput_items) {
            produced += drain_pending(output + produced, noutput_items - produced);
        }
        return produced;
    }

private:
    struct Candidate {
        int errors = 49;
        int bptc_score = 9999;
        std::size_t sync_end = 0;
        double distance = 1e30;
        int phase = 0;
        bool data_sync = false;
        std::vector<uint8_t> levels;
        std::vector<float> samples;
    };

    struct SyncMatch {
        int errors = 49;
        std::size_t end = 0;
        bool data_sync = false;
    };

    static uint8_t nearest_level(float sample)
    {
        constexpr std::array<float, 4> levels { -1.0f, -1.0f / 3.0f,
                                                1.0f / 3.0f, 1.0f };
        uint8_t best = 0;
        float best_distance = std::abs(sample - levels[0]);
        for (uint8_t index = 1; index < levels.size(); ++index) {
            const float distance = std::abs(sample - levels[index]);
            if (distance < best_distance) {
                best = index;
                best_distance = distance;
            }
        }
        return best;
    }

    static SyncMatch best_sync(const std::vector<uint8_t>& levels,
                               std::size_t min_sync_end,
                               std::size_t max_sync_end)
    {
        constexpr uint64_t mask48 = (uint64_t{ 1 } << 48) - 1;
        constexpr std::array<uint64_t, 9> patterns {
            0xDFF57D75DF5DULL, 0x755FD7DF75F7ULL, 0xD5D7F77FD757ULL,
            0x7F7D5DD57DFDULL, 0x77D55F7DFD77ULL, 0xF7FDD5DDFD55ULL,
            0xD7557F5FF7F5ULL, 0x5D577F7757FFULL, 0x7DFFD5F55D5FULL,
        };
        constexpr std::array<uint8_t, 4> dibits { 3, 2, 0, 1 };
        uint64_t shift = 0;
        SyncMatch best;
        for (std::size_t index = 0; index < levels.size(); ++index) {
            shift = ((shift << 2) | dibits[levels[index]]) & mask48;
            if (index < min_sync_end || index > max_sync_end) {
                continue;
            }
            for (const uint64_t pattern : patterns) {
                const int errors = static_cast<int>(
                    std::bitset<64>(shift ^ pattern).count());
                if (errors < best.errors) {
                    best.errors = errors;
                    best.end = index;
                    best.data_sync = pattern == 0xDFF57D75DF5DULL ||
                        pattern == 0xD5D7F77FD757ULL ||
                        pattern == 0xF7FDD5DDFD55ULL ||
                        pattern == 0xD7557F5FF7F5ULL;
                }
            }
        }
        return best;
    }

    static int bptc_parity_score(const Candidate& candidate,
                                 std::size_t start)
    {
        if (start + 132U > candidate.levels.size()) {
            return 9999;
        }
        constexpr std::array<uint8_t, 4> level_to_dibit { 3U, 2U, 0U, 1U };
        std::array<bool, 196> raw{};
        std::size_t bit = 0U;
        const auto append_dibit = [&](uint8_t level) {
            const uint8_t dibit = level_to_dibit[level];
            raw[bit++] = (dibit & 0x02U) != 0U;
            raw[bit++] = (dibit & 0x01U) != 0U;
        };
        // BPTC's 196 input bits are the first 98 burst bits and the final
        // 98 burst bits after the 68-bit sync field is removed.
        for (std::size_t index = 0U; index < 49U; ++index) {
            append_dibit(candidate.levels[start + index]);
        }
        for (std::size_t index = 83U; index < 132U; ++index) {
            append_dibit(candidate.levels[start + index]);
        }

        std::array<bool, 196> deinterleaved{};
        for (std::size_t index = 0U; index < deinterleaved.size(); ++index) {
            deinterleaved[index] = raw[(index * 181U) % 196U];
        }

        int score = 0;
        for (std::size_t row = 0U; row < 9U; ++row) {
            const bool* values = deinterleaved.data() + row * 15U + 1U;
            score += values[11] != (values[0] ^ values[1] ^ values[2] ^
                                    values[3] ^ values[5] ^ values[7] ^
                                    values[8]);
            score += values[12] != (values[1] ^ values[2] ^ values[3] ^
                                    values[4] ^ values[6] ^ values[8] ^
                                    values[9]);
            score += values[13] != (values[2] ^ values[3] ^ values[4] ^
                                    values[5] ^ values[7] ^ values[9] ^
                                    values[10]);
            score += values[14] != (values[0] ^ values[1] ^ values[2] ^
                                    values[4] ^ values[6] ^ values[7] ^
                                    values[10]);
        }
        for (std::size_t column = 0U; column < 15U; ++column) {
            std::array<bool, 13> values{};
            for (std::size_t row = 0U; row < values.size(); ++row) {
                values[row] = deinterleaved[column + 1U + row * 15U];
            }
            score += values[9] != (values[0] ^ values[1] ^ values[3] ^
                                   values[5] ^ values[6]);
            score += values[10] != (values[0] ^ values[1] ^ values[2] ^
                                    values[4] ^ values[6] ^ values[7]);
            score += values[11] != (values[0] ^ values[1] ^ values[2] ^
                                    values[3] ^ values[5] ^ values[7] ^
                                    values[8]);
            score += values[12] != (values[0] ^ values[2] ^ values[4] ^
                                    values[5] ^ values[8]);
        }
        return score;
    }

    bool select_symbols(bool search_full_burst = false)
    {
        if (burst_.size() < 800) {
            if (verbose_) {
                log("SB R<N800");
            }
            return false;
        }

        double offset_sum = 0.0;
        std::size_t offset_count = 0;
        for (const float sample : burst_) {
            if (std::abs(sample) < 2.0f) {
                offset_sum += sample;
                ++offset_count;
            }
        }
        const float rough_offset = offset_count > 0
            ? static_cast<float>(offset_sum / static_cast<double>(offset_count))
            : 0.0f;
        const float offset = have_level_calibration_ ? level_offset_ : rough_offset;
        const float scale = have_level_calibration_ ? level_scale_ : 1.0F;
        constexpr std::array<float, 4> normalized_levels {
            -1.0f, -1.0f / 3.0f, 1.0f / 3.0f, 1.0f
        };
        std::array<Candidate, 10> candidates;
        int best_sync_candidate = -1;
        int best_data_candidate = -1;
        int best_distance_candidate = -1;
        for (int phase = 0; phase < 10; ++phase) {
            Candidate& candidate = candidates[static_cast<std::size_t>(phase)];
            candidate.phase = phase;
            double squared_error = 0.0;
            for (std::size_t index = static_cast<std::size_t>(phase);
                 index < burst_.size(); index += 10) {
                const float raw_sample = burst_[index];
                const float centered = (raw_sample - offset) / scale;
                const uint8_t level = nearest_level(centered);
                candidate.levels.push_back(level);
                candidate.samples.push_back(raw_sample);
                const double error = centered - normalized_levels[level];
                squared_error += error * error;
            }
            if (candidate.levels.size() < 80) {
                continue;
            }
            const std::size_t min_sync_end = search_full_burst ? 77U : 68U;
            const std::size_t max_sync_end = search_full_burst &&
                    candidate.levels.size() >= 55U
                ? candidate.levels.size() - 55U
                : 96U;
            const SyncMatch sync = best_sync(
                candidate.levels, min_sync_end, max_sync_end);
            candidate.errors = sync.errors;
            candidate.sync_end = sync.end;
            candidate.data_sync = sync.data_sync;
            candidate.distance = squared_error / candidate.levels.size();
            if (best_distance_candidate < 0 ||
                candidate.distance < candidates[best_distance_candidate].distance) {
                best_distance_candidate = phase;
            }
            if (best_sync_candidate < 0 ||
                candidate.errors < candidates[best_sync_candidate].errors ||
                (candidate.errors == candidates[best_sync_candidate].errors &&
                 candidate.distance < candidates[best_sync_candidate].distance)) {
                best_sync_candidate = phase;
            }
            if (candidate.data_sync && candidate.errors <= 6 &&
                candidate.sync_end >= 77U) {
                const std::size_t start = candidate.sync_end - 77U;
                candidate.bptc_score = bptc_parity_score(candidate, start);
                if (best_data_candidate < 0 ||
                    candidate.bptc_score <
                        candidates[best_data_candidate].bptc_score ||
                    (candidate.bptc_score ==
                         candidates[best_data_candidate].bptc_score &&
                     (candidate.errors < candidates[best_data_candidate].errors ||
                      (candidate.errors == candidates[best_data_candidate].errors &&
                       candidate.distance <
                           candidates[best_data_candidate].distance)))) {
                    best_data_candidate = phase;
                }
            }
        }
        if (best_distance_candidate < 0) {
            return false;
        }

        const int reliable_candidate = best_data_candidate >= 0
            ? best_data_candidate : best_sync_candidate;
        const bool reliable_sync = reliable_candidate >= 0 &&
            candidates[reliable_candidate].errors <= 6 &&
            candidates[reliable_candidate].sync_end >= 77U;
        int selected_index = reliable_sync ? reliable_candidate : best_distance_candidate;
        if (!reliable_sync && have_alignment_) {
            selected_index = symbol_phase_;
        }
        Candidate& selected = candidates[static_cast<std::size_t>(selected_index)];

        if (verbose_) {
            std::ostringstream message;
            message << "SB C" << burst_.size() << " E" << selected.errors
                    << (reliable_sync ? " OK" : " NO");
            if (selected.data_sync) {
                message << " P" << selected.bptc_score;
            }
            log(message.str());
        }

        std::size_t start = alignment_start_;
        if (reliable_sync) {
            if (selected.sync_end < 77U) {
                return false;
            }
            start = selected.sync_end - 77U;
            symbol_phase_ = selected.phase;
            alignment_start_ = start;
            have_alignment_ = true;
        } else if (!have_alignment_) {
            return false;
        }
        if (start + 132U > selected.levels.size()) {
            if (reliable_sync) {
                if (verbose_) {
                    log("SB R INC");
                }
                return false;
            }
            if (selected.levels.size() < 132U) {
                return false;
            }
            start = selected.levels.size() - 132U;
        }

        if (reliable_sync) {
            double ideal_sum = 0.0;
            double sample_sum = 0.0;
            for (std::size_t index = start + 54U; index < start + 78U; ++index) {
                ideal_sum += normalized_levels[selected.levels[index]];
                sample_sum += selected.samples[index];
            }
            const double ideal_mean = ideal_sum / 24.0;
            const double sample_mean = sample_sum / 24.0;
            double covariance = 0.0;
            double ideal_variance = 0.0;
            for (std::size_t index = start + 54U; index < start + 78U; ++index) {
                const double ideal = normalized_levels[selected.levels[index]];
                covariance += (ideal - ideal_mean) *
                              (selected.samples[index] - sample_mean);
                ideal_variance += (ideal - ideal_mean) * (ideal - ideal_mean);
            }
            const float fitted_scale = ideal_variance > 0.0
                ? static_cast<float>(covariance / ideal_variance) : 1.0F;
            const float fitted_offset = static_cast<float>(
                sample_mean - fitted_scale * ideal_mean);
            if (verbose_) {
                std::ostringstream message;
                message << std::fixed << std::setprecision(2)
                        << "SB F" << fitted_scale << '/' << fitted_offset
                        << (fitted_scale > 0.2F && fitted_scale < 2.0F
                                ? " OK" : " NO");
                log(message.str());
            }
            if (fitted_scale > 0.2F && fitted_scale < 2.0F) {
                if (have_level_calibration_) {
                    level_scale_ = 0.75F * level_scale_ + 0.25F * fitted_scale;
                    level_offset_ = 0.75F * level_offset_ + 0.25F * fitted_offset;
                } else {
                    level_scale_ = fitted_scale;
                    level_offset_ = fitted_offset;
                    have_level_calibration_ = true;
                }
            }
        }

        constexpr std::array<uint8_t, 4> level_to_dibit { 3U, 2U, 0U, 1U };
        std::vector<uint8_t> dibits(132U);
        for (std::size_t index = 0; index < dibits.size(); ++index) {
            dibits[index] = level_to_dibit[selected.levels[start + index]];
        }
        message_port_pub(pmt::mp("bursts"), pmt::init_u8vector(dibits.size(), dibits));
        queue_metadata_symbols(dibits);
        if (reliable_sync) {
            arm_scheduled_capture(burst_start_absolute_ +
                                  static_cast<uint64_t>(selected.phase) +
                                  start * 10U + 2880U);
            while (scheduled_first_center_ < absolute_sample_index_) {
                arm_scheduled_capture(scheduled_first_center_ + 2880U);
            }
            missed_scheduled_bursts_ = 0;
        }
        return reliable_sync;
    }

    void process_scheduled_burst()
    {
        const uint64_t expected_next = scheduled_first_center_ + 2880U;
        std::vector<float> captured = std::move(scheduled_samples_);
        scheduled_samples_.clear();
        scheduled_ = false;
        burst_start_absolute_ = scheduled_window_start_;
        burst_ = std::move(captured);

        // A quiet slot is still a valid timing result. Advance the capture
        // clock for every miss so one idle burst cannot pin the sampler to an
        // expired window forever.
        const double magnitude = burst_.empty()
            ? 0.0
            : std::accumulate(
                  burst_.begin(), burst_.end(), 0.0,
                  [](double total, float sample) {
                      return total + std::abs(sample);
                  }) /
              static_cast<double>(burst_.size());
        if (magnitude < 0.05) {
            burst_.clear();
            if (++missed_scheduled_bursts_ >= 3U) {
                have_alignment_ = false;
                in_burst_ = false;
                pre_roll_.clear();
                quiet_run_ = 0;
                if (verbose_) {
                    log("SB RSEARCH");
                }
            } else {
                arm_scheduled_capture(expected_next);
                if (verbose_) {
                    log("SB RMISS");
                }
            }
            return;
        }

        const bool reacquired = select_symbols(true);
        burst_.clear();
        if (!reacquired && !scheduled_) {
            if (++missed_scheduled_bursts_ >= 3U) {
                have_alignment_ = false;
                in_burst_ = false;
                pre_roll_.clear();
                quiet_run_ = 0;
                if (verbose_) {
                    log("SB RSEARCH");
                }
            } else {
                arm_scheduled_capture(expected_next);
                if (verbose_) {
                    log("SB RDRIFT");
                }
            }
        }
    }

    void arm_scheduled_capture(uint64_t first_symbol_sample)
    {
        constexpr uint64_t lead = 80U;
        constexpr uint64_t burst_samples = 1320U;
        scheduled_ = true;
        scheduled_first_center_ = first_symbol_sample;
        scheduled_window_start_ = first_symbol_sample > lead
            ? first_symbol_sample - lead : 0U;
        scheduled_window_end_ = scheduled_window_start_ + burst_samples +
            lead * 2U;
        scheduled_samples_.clear();
    }

    void log(const std::string& message)
    {
        if (logger_) {
            logger_(message);
        } else {
            std::cout << message << std::endl;
        }
    }

    void queue_metadata_symbols(const std::vector<uint8_t>& dibits)
    {
        pending_.clear();
        pending_.reserve(264U);
        constexpr std::array<float, 4> dibit_to_symbol {
            1.0F / 3.0F, 1.0F, -1.0F / 3.0F, -1.0F
        };
        for (const uint8_t dibit : dibits) {
            pending_.push_back(dibit_to_symbol[dibit]);
        }
        pending_.insert(pending_.end(), 132U, 1.0F / 3.0F);
        pending_index_ = 0;
    }

    int drain_pending(float* output, int capacity)
    {
        const std::size_t remaining = pending_.size() - pending_index_;
        if (remaining == 0 || capacity == 0) {
            return 0;
        }
        const int count = std::min(capacity, static_cast<int>(remaining));
        std::copy_n(pending_.data() + pending_index_, count, output);
        pending_index_ += static_cast<std::size_t>(count);
        return count;
    }

    std::vector<float> burst_;
    std::deque<float> pre_roll_;
    std::vector<float> pending_;
    std::size_t pending_index_ = 0;
    std::size_t quiet_run_ = 0;
    uint64_t absolute_sample_index_ = 0;
    uint64_t burst_start_absolute_ = 0;
    uint64_t scheduled_first_center_ = 0;
    uint64_t scheduled_window_start_ = 0;
    uint64_t scheduled_window_end_ = 0;
    std::vector<float> scheduled_samples_;
    unsigned missed_scheduled_bursts_ = 0;
    unsigned scheduled_burst_count_ = 0;
    int symbol_phase_ = 0;
    std::size_t alignment_start_ = 0;
    bool have_alignment_ = false;
    bool have_level_calibration_ = false;
    float level_scale_ = 1.0F;
    float level_offset_ = 0.0F;
    bool scheduled_ = false;
    bool in_burst_ = false;
    bool verbose_ = false;
    LogCallback logger_;
};



} // namespace dmr_b210
