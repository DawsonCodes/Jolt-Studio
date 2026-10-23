#include "manual.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <stdexcept>

#include "report.h"
#include "timeline.h"

namespace jolt {
namespace {

// same slack the scan gives a jolt showing a frame late
constexpr double TIME_GIVE = 0.1;
constexpr size_t LISTED = 20;
constexpr int MAX_FOOD = 20;
constexpr const char* SEPARATORS = " \t\r\n,;";

std::int8_t mark_of(char letter) {
    switch (letter) {
        case '+': return -1;
        case '0': return 0;
        case '-': return 1;
        case '?': return UNREAD;
    }
    throw std::invalid_argument(std::format("'{}' is not a jolt mark, use + 0 - or ?", letter));
}

// 18 or 18H
bool is_level(const std::string& token) {
    const size_t digits = std::find_if_not(token.begin(), token.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) - token.begin();
    return digits >= 1 && digits <= 2 && (token.size() == digits || (token.size() == digits + 1 && std::toupper(token.back()) == 'H'));
}

// absorption a table assumes, hunger tables past the first also assume low health
int absorption(Row row, int skipped_draws) {
    return row == Row::Hearts ? skipped_draws : std::max(0, skipped_draws - SKIPPED_DRAWS[1]);
}

bool allowed(const WobbleTable& table, const Code& code) {
    return table.row() == Row::Hearts || (table.skipped_draws() != 0) == code.low_health;
}

bool fits(const std::vector<WobbleTable>& tables, int tick, const Code& code) {
    if (code.level >= 0 && tick % jolt_period(code.level) != 0) return false;
    return std::any_of(tables.begin(), tables.end(),
                       [&](const WobbleTable& table) { return allowed(table, code) && table.draws(tick, code.offsets); });
}

std::pair<int, int> step(const Code& from, const Code& to) {
    if (!to.timed) return {1, 1};
    const double apart = (*to.time - *from.time) * TICKS_PER_SECOND;
    return {std::max(1, int(std::floor(apart - TIME_GIVE * TICKS_PER_SECOND))),
            int(std::ceil(apart + TIME_GIVE * TICKS_PER_SECOND))};
}

}  // namespace

std::vector<Code> parse_codes(Row row, const std::string& text) {
    std::vector<Code> codes;
    Code next;
    for (size_t at = text.find_first_not_of(SEPARATORS); at != std::string::npos; at = text.find_first_not_of(SEPARATORS, at)) {
        const size_t end = std::min(text.find_first_of(SEPARATORS, at), text.size());
        const std::string token = text.substr(at, end - at);
        at = end;
        if (token.front() == '@') {
            try {
                next.time = seconds_of(token.substr(1));
            } catch (const std::logic_error&) {
                throw std::invalid_argument("cannot read the time " + token);
            }
            next.timed = true;
            continue;
        }
        if (is_level(token)) {
            if (row == Row::Hearts) throw std::invalid_argument("a heart code is the pattern alone, the food level and H are for the hunger bar");
            next.level = std::stoi(token);
            next.low_health = std::toupper(token.back()) == 'H';
            if (next.level > MAX_FOOD) throw std::invalid_argument(std::format("food level {} is more than 20", next.level));
            continue;
        }
        if (token.size() != size_t(SHANKS)) {
            throw std::invalid_argument(std::format("{} is not a jolt code, a code is 10 marks like -000+0--+0", token));
        }
        if (row == Row::Food && next.level < 0) {
            throw std::invalid_argument("a hunger code starts with the food level, like 18H -000+0--+0");
        }
        std::array<std::int8_t, SHANKS> typed;
        std::transform(token.begin(), token.end(), typed.begin(), mark_of);
        if (row == Row::Hearts && std::count(typed.begin(), typed.end(), -1)) {
            throw std::invalid_argument("hearts only drop, + is for the hunger bar");
        }
        // typed left to right, the game draws right to left
        std::reverse_copy(typed.begin(), typed.end(), next.offsets.begin());
        if (next.timed && !codes.empty() && !codes.front().time) {
            throw std::invalid_argument("a code with a time cannot follow codes without one, give the first code a time too");
        }
        if (next.timed && !codes.empty() && *next.time <= *codes.back().time) {
            throw std::invalid_argument("the codes go in the order the video shows them, each time later than the one before");
        }
        // no time means the tick after the previous code
        if (!next.time && !codes.empty() && codes.back().time) next.time = *codes.back().time + 1.0 / TICKS_PER_SECOND;
        codes.push_back(next);
        next = {};
    }
    if (next.timed || next.level >= 0) throw std::invalid_argument("a time or food level has no code after it");
    if (codes.empty()) throw std::invalid_argument("no jolt code given");
    return codes;
}

std::vector<Crack> crack(Row row, const std::vector<Code>& codes, int max_tick) {
    std::vector<WobbleTable> tables;
    if (row == Row::Food) {
        for (int draws : SKIPPED_DRAWS) {
            const bool wanted = std::any_of(codes.begin(), codes.end(), [&](const Code& code) { return (draws != 0) == code.low_health; });
            if (wanted) tables.emplace_back(Row::Food, max_tick, draws);
        }
    } else {
        for (int draws : HEARTS_ABOVE) tables.emplace_back(Row::Hearts, max_tick, draws);
    }
    std::vector<Crack> found;
    for (const WobbleTable& table : tables) {
        if (!allowed(table, codes.front())) continue;
        for (int tick : table.ticks_matching(codes.front().offsets)) {
            if (codes.front().level >= 0 && tick % jolt_period(codes.front().level) != 0) continue;
            std::vector<int> places = {tick};
            for (size_t index = 1; index < codes.size() && !places.empty(); ++index) {
                const auto [least, most] = step(codes[index - 1], codes[index]);
                std::vector<int> further;
                for (int place : places) {
                    for (int ahead = least; ahead <= most; ++ahead) {
                        if (fits(tables, place + ahead, codes[index])) further.push_back(place + ahead);
                    }
                }
                std::sort(further.begin(), further.end());
                further.erase(std::unique(further.begin(), further.end()), further.end());
                places = std::move(further);
            }
            if (!places.empty()) found.push_back({tick, table.skipped_draws()});
        }
    }
    std::sort(found.begin(), found.end(), [&](const Crack& a, const Crack& b) {
        return std::pair(absorption(row, a.skipped_draws) != 0, a.tick) < std::pair(absorption(row, b.skipped_draws) != 0, b.tick);
    });
    return found;
}

std::string describe_cracks(Row row, const std::vector<Code>& codes, const std::vector<Crack>& cracks, double max_hours) {
    std::string out = std::format("{} {} jolt code{}, looked up over {:g} hours of game uptime\n\n", codes.size(),
                                  row == Row::Food ? "hunger" : "heart", codes.size() > 1 ? "s" : "", max_hours);
    if (cracks.empty()) {
        return out + std::format("No tick in the first {:g} hours draws these codes. Check each mark left to right, the food "
                                 "level and H, and the times between the codes. A game open longer than that needs a higher "
                                 "longest uptime.\n",
                                 max_hours);
    }
    if (cracks.size() == 1) {
        out += "Cracked, one tick fits.";
    } else if (row == Row::Hearts && codes.size() == 1) {
        out += std::format("{} ticks fit. Ten hearts make only 1024 patterns, so one code is never enough: step a frame "
                           "on and type the hearts of the next tick after it.", cracks.size());
    } else {
        out += std::format("{} ticks fit. Another jolt with its time narrows it down.", cracks.size());
    }
    out += " Game uptime at the first code:\n";
    const std::optional<double> start = codes.front().time;
    for (size_t index = 0; index < std::min(cracks.size(), LISTED); ++index) {
        const Crack& one = cracks[index];
        const double uptime = double(one.tick) / TICKS_PER_SECOND;
        out += std::format("  {} ({} ticks)", clock_text(uptime, 2), one.tick);
        if (start) out += ", video began at " + clock_text(std::max(0.0, uptime - *start), 2);
        if (const int extra = absorption(row, one.skipped_draws)) out += std::format(", with {} absorption heart{}", extra, extra > 1 ? "s" : "");
        out += "\n";
    }
    if (cracks.size() > LISTED) out += std::format("  and {} more\n", cracks.size() - LISTED);
    return out;
}

std::string crack_verdict(const std::vector<Code>& codes, const std::vector<Crack>& cracks) {
    if (cracks.empty()) return "No tick fits these jolt codes";
    if (cracks.size() > 1) return std::format("{} ticks fit, add another jolt to narrow it down", cracks.size());
    const double uptime = double(cracks.front().tick) / TICKS_PER_SECOND;
    if (codes.front().time) return "Cracked: game uptime " + clock_text(std::max(0.0, uptime - *codes.front().time), 2) + " when the video began";
    return "Cracked: game uptime " + clock_text(uptime, 2) + " at the first code";
}

}  // namespace jolt
