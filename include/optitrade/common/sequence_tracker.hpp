// Author: Harsh
#pragma once

#include <cstdint>

namespace optitrade {

// Tracks the feed-wide sequence number. Allocation-free.
class SequenceTracker {
public:
    // Returns true if expected (last_seen + 1) or the very first packet.
    [[nodiscard]] bool check_and_update(const std::uint32_t sequence_num) noexcept {
        const bool ok = !seen_ || sequence_num == last_seen_ + 1;
        if (!ok) {
            ++gap_count_;
        }
        seen_ = true;
        last_seen_ = sequence_num;
        return ok;
    }

    [[nodiscard]] std::uint32_t get_total_gap_count() const noexcept {
        return gap_count_;
    }

private:
    std::uint32_t last_seen_{};
    std::uint32_t gap_count_{};
    bool seen_{};
};

}  // namespace optitrade
