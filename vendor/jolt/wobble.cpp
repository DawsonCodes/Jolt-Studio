#include "wobble.h"

#include <algorithm>
#include <bit>
#include <climits>

#include "parallel.h"

namespace jolt {
namespace {

constexpr std::uint64_t MULTIPLIER = 0x5DEECE66DULL;
constexpr std::uint64_t INCREMENT = 0xB;
constexpr std::uint64_t MASK = (1ULL << 48) - 1;
constexpr std::uint32_t SEED_FACTOR = 312871;

// java.util.Random, the hud reseeds it from the tick counter every frame
class JavaRandom {
public:
    explicit JavaRandom(std::int64_t seed) : state_((static_cast<std::uint64_t>(seed) ^ MULTIPLIER) & MASK) {}

    int next_int(int bound) {
        if ((bound & (bound - 1)) == 0) return static_cast<int>((bound * next31()) >> 31);
        for (;;) {
            const std::int64_t sample = next31();
            // java redraws when this overflows an int
            if (sample - sample % bound + bound - 1 <= INT_MAX) return static_cast<int>(sample % bound);
        }
    }

private:
    std::int64_t next31() {
        state_ = (state_ * MULTIPLIER + INCREMENT) & MASK;
        return static_cast<std::int64_t>(state_ >> 17);
    }

    std::uint64_t state_;
};

int ways(Row row) { return row == Row::Food ? 3 : 2; }
int highest(Row row) { return row == Row::Food ? -1 : 0; }

int pattern_code(Row row, int tick, int skipped_draws) {
    // the game multiplies as a 32 bit int so it wraps
    const auto seed = static_cast<std::int32_t>(static_cast<std::uint32_t>(tick) * SEED_FACTOR);
    JavaRandom random(seed);
    for (int draw = 0; draw < skipped_draws; ++draw) random.next_int(2);
    int code = 0;
    for (int shank = 0, place = 1; shank < SHANKS; ++shank, place *= ways(row)) {
        code += random.next_int(ways(row)) * place;
    }
    return code;
}

}  // namespace

WobbleTable::WobbleTable(Row row, int max_tick, int skipped_draws)
    : row_(row), skipped_draws_(skipped_draws), codes_(max_tick + 1), by_code_(max_tick + 1) {
    int codes = 1;
    for (int shank = 0; shank < SHANKS; ++shank) codes *= ways(row);
    first_.assign(codes + 1, 0);
    // the rng per tick is the slow part and ticks are independent
    const int parts = all_cores();
    for_each_index(parts, parts, [&](int part) {
        const auto from = int(std::int64_t(max_tick + 1) * part / parts);
        const auto to = int(std::int64_t(max_tick + 1) * (part + 1) / parts);
        for (int tick = from; tick < to; ++tick) {
            codes_[tick] = static_cast<std::uint16_t>(pattern_code(row, tick, skipped_draws));
        }
    });
    for (int tick = 0; tick <= max_tick; ++tick) ++first_[codes_[tick] + 1];
    for (int code = 0; code < codes; ++code) first_[code + 1] += first_[code];
    std::vector<std::int32_t> next(first_.begin(), first_.end() - 1);
    for (int tick = 0; tick <= max_tick; ++tick) by_code_[next[codes_[tick]]++] = tick;
    if (row != Row::Hearts) return;

    // and a spare word, 64 ticks are read from wherever a link falls
    const int words = max_tick / 64 + 2;
    down_.assign(SHANKS, std::vector<std::uint64_t>(words));
    for_each_index(parts, parts, [&](int part) {
        for (int word = words * part / parts; word < words * (part + 1) / parts; ++word) {
            for (int tick = word * 64; tick < std::min(word * 64 + 64, max_tick + 1); ++tick) {
                for (int shank = 0; shank < SHANKS; ++shank) {
                    down_[shank][word] |= std::uint64_t(codes_[tick] >> shank & 1) << (tick & 63);
                }
            }
        }
    });
}

Offsets WobbleTable::offsets(int tick) const {
    Offsets offsets;
    int code = codes_[tick];
    for (int shank = 0; shank < SHANKS; ++shank, code /= ways(row_)) {
        offsets[shank] = static_cast<std::int8_t>(code % ways(row_) + highest(row_));
    }
    return offsets;
}

bool WobbleTable::shows(int code, const Offsets& offsets) const {
    for (int shank = 0; shank < SHANKS; ++shank, code /= ways(row_)) {
        if (offsets[shank] != UNREAD && offsets[shank] != code % ways(row_) + highest(row_)) return false;
    }
    return true;
}

bool WobbleTable::draws(int tick, const Offsets& offsets) const {
    if (tick < 0 || tick >= static_cast<int>(codes_.size())) return false;
    return shows(codes_[tick], offsets);
}

std::vector<int> WobbleTable::ticks_matching(const Offsets& offsets) const {
    std::vector<int> codes = {0};
    for (int shank = 0, place = 1; shank < SHANKS; ++shank, place *= ways(row_)) {
        std::vector<int> longer;
        for (int code : codes) {
            for (int digit = 0; digit < ways(row_); ++digit) {
                const bool fits = offsets[shank] == UNREAD || offsets[shank] == digit + highest(row_);
                if (fits) longer.push_back(code + digit * place);
            }
        }
        codes = std::move(longer);
    }

    std::vector<int> ticks;
    for (int code : codes) ticks.insert(ticks.end(), by_code_.begin() + first_[code], by_code_.begin() + first_[code + 1]);
    if (codes.size() > 1) std::sort(ticks.begin(), ticks.end());
    return ticks;
}

std::vector<int> WobbleTable::ticks_matching(const std::vector<Link>& chain, bool one_missed) const {
    struct Read {
        int ticks, shank;
        bool down;
    };
    std::vector<Read> reads;
    std::vector<size_t> ends;
    int longest = 0;
    for (const Link& link : chain) {
        for (int shank = 0; shank < SHANKS; ++shank) {
            if (link.offsets[shank] != UNREAD) reads.push_back({link.ticks, shank, link.offsets[shank] == 1});
        }
        ends.push_back(reads.size());
        longest = std::max(longest, link.ticks);
    }
    const auto from = [](const std::vector<std::uint64_t>& row, int tick) {
        const int word = tick / 64, shift = tick % 64;
        return shift ? row[word] >> shift | row[word + 1] << (64 - shift) : row[word];
    };

    // 64 first ticks at a time, a bit stays set while its chain fits
    std::vector<int> ticks;
    const int starts = int(codes_.size()) - longest;
    for (int first = 0; first < starts; first += 64) {
        std::uint64_t all = starts - first < 64 ? (std::uint64_t(1) << (starts - first)) - 1 : ~std::uint64_t(0);
        std::uint64_t nearly = all;
        for (size_t link = 0, read = 0; link < ends.size() && (one_missed ? nearly : all); ++link) {
            std::uint64_t fits = ~std::uint64_t(0);
            for (; read < ends[link]; ++read) {
                const std::uint64_t down = from(down_[reads[read].shank], first + reads[read].ticks);
                fits &= reads[read].down ? down : ~down;
            }
            nearly = (nearly & fits) | all;
            all &= fits;
        }
        for (std::uint64_t hits = one_missed ? nearly : all; hits; hits &= hits - 1) {
            ticks.push_back(first + std::countr_zero(hits));
        }
    }
    return ticks;
}

}  // namespace jolt
