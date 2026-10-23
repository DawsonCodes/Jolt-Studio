#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace jolt {

inline constexpr int SHANKS = 10;
inline constexpr int TICKS_PER_SECOND = 20;
inline constexpr std::int8_t UNREAD = 9;

// shanks go up or down a gui pixel, hearts only down
enum class Row { Food, Hearts };

// -1 up, 0 level, 1 down, rightmost first
using Offsets = std::array<std::int8_t, SHANKS>;

inline int jolt_period(int food) { return food * 3 + 1; }

// offsets seen this many ticks after the first of a chain
struct Link {
    int ticks;
    Offsets offsets;
};

class WobbleTable {
public:
    // skipped_draws, what the hearts used up before this row
    WobbleTable(Row row, int max_tick, int skipped_draws);

    Row row() const { return row_; }
    int skipped_draws() const { return skipped_draws_; }
    Offsets offsets(int tick) const;
    // UNREAD matches anything, false outside the table
    bool draws(int tick, const Offsets& offsets) const;
    std::vector<int> ticks_matching(const Offsets& offsets) const;
    // first ticks of every chain that draws all the links, or all but one, hearts only
    std::vector<int> ticks_matching(const std::vector<Link>& chain, bool one_missed = false) const;

private:
    bool shows(int code, const Offsets& offsets) const;

    Row row_;
    int skipped_draws_;
    std::vector<std::uint16_t> codes_;
    std::vector<std::int32_t> by_code_;
    std::vector<std::int32_t> first_;
    // hearts only, per shank a bit for every tick it's down
    std::vector<std::vector<std::uint64_t>> down_;
};

}  // namespace jolt
