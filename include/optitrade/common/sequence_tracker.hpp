// Author: Harsh
#pragma once

#include <cstdint>
#include <unordered_map>

namespace optitrade {

class SequenceTracker {
public:
    SequenceTracker() = default;

    // Returns true if expected (last_seen + 1) or first packet for this symbol
    [[nodiscard]] bool check_and_update(const std::uint32_t symbol_id,
                                        const std::uint32_t sequence_num) noexcept {
        auto it = last_seen_.find(symbol_id);
        if (it == last_seen_.end()) {
            last_seen_[symbol_id] = sequence_num;
            return true;
        }

        const std::uint32_t expected = it->second + 1;
        it->second = sequence_num; // Update to the newly seen sequence

        if (sequence_num != expected) {
            gap_counts_[symbol_id]++;
            return false; // Gap detected
        }
        return true;
    }

    [[nodiscard]] std::uint32_t get_gap_count(const std::uint32_t symbol_id) const noexcept {
        auto it = gap_counts_.find(symbol_id);
        return it != gap_counts_.end() ? it->second : 0;
    }

    [[nodiscard]] std::uint32_t get_total_gap_count() const noexcept {
        std::uint32_t total = 0;
        for (const auto& [_, count] : gap_counts_) {
            total += count;
        }
        return total;
    }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> last_seen_;
    std::unordered_map<std::uint32_t, std::uint32_t> gap_counts_;
};

}  // namespace optitrade
