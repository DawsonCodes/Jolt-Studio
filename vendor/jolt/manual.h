#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wobble.h"

namespace jolt {

// hand typed jolt code, syntax is in the guide
struct Code {
    Offsets offsets;
    // none if no earlier code had a time
    std::optional<double> time;
    bool timed = false;
    // half shanks, -1 for hearts
    int level = -1;
    // the H, health was 2 hearts or less so the hearts drew first
    bool low_health = false;
};

std::vector<Code> parse_codes(Row row, const std::string& text);

struct Crack {
    int tick;
    int skipped_draws;
};

// no absorption needed first, then by tick
std::vector<Crack> crack(Row row, const std::vector<Code>& codes, int max_tick);

std::string describe_cracks(Row row, const std::vector<Code>& codes, const std::vector<Crack>& cracks, double max_hours);

std::string crack_verdict(const std::vector<Code>& codes, const std::vector<Crack>& cracks);

}  // namespace jolt
