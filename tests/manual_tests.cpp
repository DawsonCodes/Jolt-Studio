#include "manual.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// 24h, the --max-hours default
constexpr int MAX_TICK = 24 * 3600 * 20;

std::vector<jolt::Crack> cracks(jolt::Row row, const std::string& text) {
    return jolt::crack(row, jolt::parse_codes(row, text), MAX_TICK);
}

int only_tick(jolt::Row row, const std::string& text) {
    const auto found = cracks(row, text);
    if (found.size() != 1) throw std::runtime_error(std::to_string(found.size()) + " ticks fit " + text);
    return found.front().tick;
}

bool rejected(jolt::Row row, const std::string& text) {
    try { jolt::parse_codes(row, text); } catch (const std::invalid_argument&) { return true; }
    return false;
}
}  // namespace

int main() {
    using jolt::Row;
    int checks = 0;
    const auto expect = [&](bool good, const char* what) {
        if (!good) throw std::runtime_error(what);
        ++checks;
    };
    // the first two hunger jolts of small.mp4 and the first three heart jolts of native.mp4
    expect(only_tick(Row::Food, "@0.1 20 +--+000-++ @3.1667 20 -0+++0+--+") == 6222, "two timed hunger jolts");
    const auto alone = cracks(Row::Food, "20 +--+000-++");
    expect(!alone.empty() && alone.front().tick == 6222, "one hunger code puts the right tick first");
    for (const auto& fit : cracks(Row::Food, "19 +--+000-++")) expect(fit.tick != 6222, "a wrong food level rules the tick out");
    for (const auto& fit : cracks(Row::Food, "20H +--+000-++")) expect(fit.tick != 6222, "H rules out ticks drawn at full health");
    expect(only_tick(Row::Hearts, "0--------0 -00-0--0-0 0-0000-0-0") == 25926, "three heart ticks in a row");
    expect(only_tick(Row::Hearts, "@58.75 0--------0 @58.8 -00-0--0-0 @58.8667 0-0000-0-0") == 25926, "timed heart jolts");
    expect(only_tick(Row::Hearts, "@58.75 0--------0 -00-0--0-0 0-0000-0-0") == 25926, "untimed after timed");

    expect(rejected(Row::Food, "+--+000-++"), "hunger code without a food level");
    expect(rejected(Row::Food, "20 +--+0"), "short code");
    expect(rejected(Row::Food, "20 +--+000-+x"), "unknown mark");
    expect(rejected(Row::Food, "21 +--+000-++"), "food level too high");
    expect(rejected(Row::Hearts, "+000000000"), "hearts never go up");
    expect(rejected(Row::Hearts, "4 0000000000"), "food level on hearts");
    expect(rejected(Row::Food, "@3 20 0000000000 @1 20 0000000000"), "times out of order");
    expect(rejected(Row::Food, "20 0000000000 @1 20 0000000000"), "time after untimed codes");
    expect(rejected(Row::Food, "20 0000000000 @1"), "time with no code");
    expect(rejected(Row::Food, ""), "nothing typed");
    std::printf("%d jolt code checks pass.\n", checks);
    return 0;
}
