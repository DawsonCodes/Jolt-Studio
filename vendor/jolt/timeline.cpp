#include "timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

#include "parallel.h"

namespace jolt {
namespace {

// low food in the dark leaves only the full shanks readable, the food level's period rules out the rest
constexpr int MIN_PERIODIC_SHANKS = 6;
constexpr double BREAK_COST = 6.0;
constexpr double SKIP_COST = 4.0;
constexpr double WRONG_PERIOD_COST = 2.0;
constexpr double LOW_HEALTH_COST = 1.0;
constexpr double ABSORPTION_COST = 1.0;
// two misread heart jolts always fit each other somewhere, that has to cost more than skipping both
constexpr double ONE_SIDED_COST = 2.0;
constexpr double MAX_NEIGHBOUR_SECONDS = 0.25;
// junk lines up this well less than 1 in 10000
constexpr double MAX_ACCIDENT_ODDS = 1e-4;
// with this many bits besides its fullest link a chain fits all but one link by chance less than once in ten,
// over 24 hours and three tables
constexpr int MIN_NEARLY_BITS = 30;

struct Candidate {
    int tick;
    double price;
    int skipped_draws;
    int rivals = 0;
    // how many ticks into a chain its jolts get its tick
    int covers = std::numeric_limits<int>::max();
};

int read_shanks(const Offsets& offsets) {
    return int(std::count_if(offsets.begin(), offsets.end(), [](auto offset) { return offset != UNREAD; }));
}

bool readable(const Jolt& jolt) { return read_shanks(jolt.offsets) >= MIN_READ_SHANKS; }

const Jolt* neighbour(const std::vector<Jolt>& jolts, int index, int step) {
    for (int at = index + step; at >= 0 && at < int(jolts.size()); at += step) {
        if (std::abs(jolts[at].time - jolts[index].time) > MAX_NEIGHBOUR_SECONDS) break;
        if (jolts[at].row == Row::Hearts && readable(jolts[at])) return &jolts[at];
    }
    return nullptr;
}

bool backed(const WobbleTable& table, int tick, const Jolt& jolt, const Jolt* other) {
    if (!other) return false;
    const double apart = (other->time - jolt.time) * TICKS_PER_SECOND;
    // jolts can show a frame late so the gap can round either way
    for (const int ticks : {int(std::floor(apart)), int(std::ceil(apart))}) {
        if (ticks != 0 && table.draws(tick + ticks, other->offsets)) return true;
    }
    return false;
}

void count_rivals(std::vector<Candidate>& found, const std::map<double, int>& fitting) {
    for (Candidate& candidate : found) {
        for (const auto& [price, ticks] : fitting) {
            if (price <= candidate.price) candidate.rivals += ticks;
        }
    }
}

std::vector<Candidate> candidates(const std::vector<Jolt>& jolts, int index, const std::vector<WobbleTable>& tables) {
    const Jolt& jolt = jolts[index];
    std::vector<Candidate> found;
    const bool hearts = jolt.row == Row::Hearts;
    const int read = read_shanks(jolt.offsets);
    const bool periodic = !hearts && jolt.level >= 0 && read >= MIN_PERIODIC_SHANKS;
    if (read < MIN_READ_SHANKS && !periodic) return found;

    const Jolt* before = hearts ? neighbour(jolts, index, -1) : nullptr;
    const Jolt* after = hearts ? neighbour(jolts, index, 1) : nullptr;
    std::map<double, int> fitting;
    for (const WobbleTable& table : tables) {
        if (table.row() != jolt.row) continue;
        for (int tick : table.ticks_matching(jolt.offsets)) {
            double price = table.skipped_draws() ? (hearts ? ABSORPTION_COST : LOW_HEALTH_COST) : 0.0;
            if (!hearts && jolt.level >= 0 && tick % jolt_period(jolt.level) != 0) {
                if (read < MIN_READ_SHANKS) continue;
                price += WRONG_PERIOD_COST;
            }
            ++fitting[price];
            if (hearts) {
                // 10 hearts are only 1024 patterns, the neighbour isn't independent so every fit stays a rival
                const int backing = backed(table, tick, jolt, before) + backed(table, tick, jolt, after);
                if (backing == 0) continue;
                if (backing == 1) price += ONE_SIDED_COST;
            }
            found.push_back({tick, price, table.skipped_draws()});
        }
    }
    count_rivals(found, fitting);
    return found;
}

// a chain's jolt with only the hearts read on frames that agree
Offsets sure_of(const Jolt& jolt) {
    Offsets sure = jolt.offsets;
    for (int slot = 0; slot < SHANKS; ++slot) {
        if (jolt.unsure >> slot & 1) sure[slot] = UNREAD;
    }
    return sure;
}

int read_bits(const std::vector<Link>& chain) {
    return std::accumulate(chain.begin(), chain.end(), 0,
                           [](int sum, const Link& link) { return sum + read_shanks(link.offsets); });
}

std::vector<Link> only(const std::vector<Link>& chain, int slot) {
    std::vector<Link> alone;
    for (const Link& link : chain) {
        if (link.offsets[slot] == UNREAD) continue;
        Offsets one;
        one.fill(UNREAD);
        one[slot] = link.offsets[slot];
        alone.push_back({link.ticks, one});
    }
    return alone;
}

int fullest(const std::vector<Link>& chain) {
    int most = 0;
    for (const Link& link : chain) most = std::max(most, read_shanks(link.offsets));
    return most;
}

// a chain has bits enough to place itself, so it needs no neighbours
std::vector<Candidate> chain_candidates(const std::vector<Jolt>& jolts, const std::vector<int>& members,
                                        const std::vector<WobbleTable>& tables) {
    std::vector<Link> all, sure;
    for (int member : members) {
        const Jolt& jolt = jolts[member];
        all.push_back({jolt.link, jolt.offsets});
        const Offsets steady = sure_of(jolt);
        if (read_shanks(steady)) sure.push_back({jolt.link, steady});
    }
    std::vector<std::vector<Link>> tries;
    std::vector<int> covering;
    const auto try_stretch = [&](const std::vector<Link>& stretch, const std::vector<Link>& sure_stretch, int covers) {
        const int bits = read_bits(stretch), sure_bits = read_bits(sure_stretch);
        if (bits >= MIN_CHAIN_BITS) tries.push_back(stretch);
        // a frame still showing the tick before spoils its link, so the chain is also placed by the hearts read on
        // frames that agree
        if (sure_bits >= MIN_CHAIN_BITS && sure_bits < bits) tries.push_back(sure_stretch);
        // one misread heart spoils the whole chain, so a heart read through enough of it is also matched alone
        for (int slot = 0; slot < SHANKS; ++slot) {
            std::vector<Link> alone = only(stretch, slot), steady = only(sure_stretch, slot);
            const int size = int(alone.size()), steady_size = int(steady.size());
            if (size >= MIN_CHAIN_BITS && size < bits) tries.push_back(std::move(alone));
            if (steady_size >= MIN_CHAIN_BITS && steady_size < size && steady_size < sure_bits) {
                tries.push_back(std::move(steady));
            }
        }
        covering.resize(tries.size(), covers);
    };
    try_stretch(all, sure, std::numeric_limits<int>::max());
    // a chain longer than CHAIN_SECONDS is also placed by its first CHAIN_SECONDS, the rest of the run it took in
    // may be misread or past a cut, and then only that stretch gets its tick
    const int span = int(CHAIN_SECONDS * TICKS_PER_SECOND);
    if (all.back().ticks >= span) {
        const auto head = [&](const std::vector<Link>& chain) {
            return std::vector<Link>(chain.begin(), std::find_if(chain.begin(), chain.end(), [&](const Link& link) {
                return link.ticks >= span;
            }));
        };
        try_stretch(head(all), head(sure), span);
    }

    std::vector<std::vector<int>> fits(tries.size() * tables.size());
    for (size_t at = 0; at < tries.size(); ++at) {
        for (size_t table = 0; table < tables.size(); ++table) {
            if (tables[table].row() != Row::Hearts) continue;
            fits[at * tables.size() + table] = tables[table].ticks_matching(tries[at]);
        }
    }
    // a try that fits nowhere may hold a link misread on frames that agree, rare enough to allow one when the rest
    // fit by chance so seldom
    for (size_t at = 0; at < tries.size(); ++at) {
        if (read_bits(tries[at]) - fullest(tries[at]) < MIN_NEARLY_BITS) continue;
        const auto row = fits.begin() + at * tables.size();
        if (std::any_of(row, row + tables.size(), [](const std::vector<int>& fit) { return !fit.empty(); })) continue;
        for (size_t table = 0; table < tables.size(); ++table) {
            if (tables[table].row() == Row::Hearts) row[table] = tables[table].ticks_matching(tries[at], true);
        }
    }

    std::vector<Candidate> found;
    std::map<double, int> fitting;
    for (size_t table = 0; table < tables.size(); ++table) {
        if (tables[table].row() != Row::Hearts) continue;
        const double price = tables[table].skipped_draws() ? ABSORPTION_COST : 0.0;
        std::map<int, int> ticks;
        for (size_t at = 0; at < tries.size(); ++at) {
            for (int tick : fits[at * tables.size() + table]) ticks[tick] = std::max(ticks[tick], covering[at]);
        }
        for (const auto& [tick, covers] : ticks) {
            ++fitting[price];
            found.push_back({tick, price, tables[table].skipped_draws(), 0, covers});
        }
    }
    count_rivals(found, fitting);
    return found;
}

std::vector<Run> split(const std::vector<Fix>& fixes, double tolerance) {
    std::vector<Run> runs;
    for (const Fix& fix : fixes) {
        const bool steady = !runs.empty() && std::abs(fix.uptime_at_video_start() -
                                                      runs.back().back().uptime_at_video_start()) <= tolerance;
        if (!steady) runs.emplace_back();
        runs.back().push_back(fix);
    }
    return runs;
}

double accident_odds(const Run& run, int max_tick, double tolerance) {
    const double window = 2 * tolerance * TICKS_PER_SECOND + 1;
    double odds = run.front().rivals;
    for (size_t index = 1; index < run.size(); ++index) {
        if (!run[index].chained) odds *= run[index].rivals * window / max_tick;
    }
    return odds;
}

}  // namespace

std::vector<std::optional<Fix>> decode(const std::vector<Jolt>& jolts, const std::vector<WobbleTable>& tables,
                                       double tolerance) {
    struct Node {
        int parent;
        Fix fix;
        int covers;
    };
    // tick -1 means no fix yet
    struct State {
        double cost;
        int tick;
        double time;
        int node;
    };
    std::vector<Node> nodes;
    std::vector<State> states = {{0.0, -1, 0.0, -1}};

    // a chain is decoded once, at its first jolt
    std::map<int, std::vector<int>> chains;
    for (int index = 0; index < int(jolts.size()); ++index) {
        if (jolts[index].chain >= 0) chains[jolts[index].chain].push_back(index);
    }
    const auto leads = [&](int index) { return jolts[index].chain < 0 || chains.at(jolts[index].chain).front() == index; };

    // lookups are most of the work and independent, spread them over all cores
    std::vector<std::vector<Candidate>> looked_up(jolts.size());
    for_each_index(int(jolts.size()), all_cores(), [&](int index) {
        if (jolts[index].chain < 0) looked_up[index] = candidates(jolts, index, tables);
        else if (leads(index)) looked_up[index] = chain_candidates(jolts, chains.at(jolts[index].chain), tables);
    });

    for (int index = 0; index < int(jolts.size()); ++index) {
        if (!leads(index)) continue;
        const Jolt& jolt = jolts[index];
        const std::vector<Candidate>& found = looked_up[index];

        std::vector<State> next;
        for (const State& state : states) next.push_back({state.cost + SKIP_COST, state.tick, state.time, state.node});
        for (const Candidate& candidate : found) {
            double reach = 0;
            int parent = -1;
            for (const State& state : states) {
                const double drift = double(candidate.tick - state.tick) / TICKS_PER_SECOND - (jolt.time - state.time);
                const bool smooth = state.tick < 0 || std::abs(drift) <= tolerance;
                const double cost = state.cost + (smooth ? 0.0 : BREAK_COST);
                if (parent < 0 || cost < reach) {
                    reach = cost;
                    parent = state.node;
                }
            }
            const Fix fix{index, jolt.time, candidate.tick, candidate.skipped_draws, candidate.rivals};
            nodes.push_back({parent, fix, candidate.covers});
            next.push_back({reach + candidate.price, candidate.tick, jolt.time, int(nodes.size()) - 1});
        }

        // anything further behind loses to just breaking from the leader
        double lead = next.front().cost;
        for (const State& state : next) lead = std::min(lead, state.cost);
        states.clear();
        for (const State& state : next) {
            if (state.cost <= lead + BREAK_COST) states.push_back(state);
        }
    }

    const auto winner = std::min_element(states.begin(), states.end(),
                                         [](const State& a, const State& b) { return a.cost < b.cost; });
    std::vector<std::optional<Fix>> fixes(jolts.size());
    for (int at = winner->node; at >= 0; at = nodes[at].parent) {
        const Fix& fix = nodes[at].fix;
        if (jolts[fix.jolt].chain < 0) {
            fixes[fix.jolt] = fix;
            continue;
        }
        // a chain placed by one heart can hold jolts misread elsewhere, those don't get its tick
        const WobbleTable& table = *std::find_if(tables.begin(), tables.end(), [&](const WobbleTable& one) {
            return one.row() == Row::Hearts && one.skipped_draws() == fix.skipped_draws;
        });
        bool first = true;
        for (int member : chains.at(jolts[fix.jolt].chain)) {
            const Jolt& jolt = jolts[member];
            if (jolt.link >= nodes[at].covers) break;
            // hearts read on frames that agree must fit, the rest may show the tick before but no more of them may
            // miss than fit
            if (!table.draws(fix.tick + jolt.link, sure_of(jolt))) continue;
            int fit = 0;
            for (int slot = 0; slot < SHANKS; ++slot) {
                if (jolt.offsets[slot] == UNREAD) continue;
                Offsets one;
                one.fill(UNREAD);
                one[slot] = jolt.offsets[slot];
                fit += table.draws(fix.tick + jolt.link, one) ? 1 : -1;
            }
            if (fit < 0) continue;
            fixes[member] = Fix{member, jolt.time, fix.tick + jolt.link, fix.skipped_draws, fix.rivals, !first};
            first = false;
        }
    }
    return fixes;
}

std::vector<Run> confirmed_runs(const std::vector<std::optional<Fix>>& fixes, int max_tick, double tolerance) {
    std::vector<Fix> found;
    for (const auto& fix : fixes) {
        if (fix) found.push_back(*fix);
    }
    std::vector<Fix> kept;
    for (const Run& run : split(found, tolerance)) {
        if (accident_odds(run, max_tick, tolerance) <= MAX_ACCIDENT_ODDS) kept.insert(kept.end(), run.begin(), run.end());
    }
    return split(kept, tolerance);
}

std::vector<Run> runs_in_order(const std::vector<std::optional<Fix>>& fixes, const std::vector<Jolt>& jolts,
                               double tolerance) {
    std::vector<Run> runs;
    for (const auto& fix : fixes) {
        if (!fix) continue;
        const bool carries_on = !runs.empty() && jolts[fix->jolt].screen == jolts[runs.back().back().jolt].screen &&
                                std::abs(fix->uptime_at_video_start() - runs.back().back().uptime_at_video_start()) <= tolerance;
        if (!carries_on) runs.emplace_back();
        runs.back().push_back(*fix);
    }
    return runs;
}

Games sort_into_games(const std::vector<Run>& runs, const std::vector<Jolt>& jolts, double tolerance) {
    Games games;
    std::vector<Fix> last;
    const auto screen = [&](const Fix& fix) { return jolts[fix.jolt].screen; };
    for (size_t index = 0; index < runs.size(); ++index) {
        const Fix& first = runs[index].front();
        int game = -1;
        for (int known = 0; known < int(last.size()); ++known) {
            // not lower, a game paused on a jolt shows the same tick again
            const bool fits = screen(first) == screen(last[known]) && first.tick >= last[known].tick &&
                              first.uptime_at_video_start() <= last[known].uptime_at_video_start() + tolerance;
            if (fits && (game < 0 || last[known].uptime_at_video_start() < last[game].uptime_at_video_start())) {
                game = known;
            }
        }
        int earlier = int(index) - 1;
        while (earlier >= 0 && screen(runs[earlier].front()) != screen(first)) --earlier;

        Fix before = index > 0 ? runs[index - 1].back() : first;
        BreakKind kind;
        if (game >= 0) {
            kind = game + 1 == games.of_run.back() ? BreakKind::Stalled : BreakKind::Back;
        } else if (earlier >= 0 && first.tick > runs[earlier].back().tick) {
            game = games.of_run[earlier] - 1;
            kind = BreakKind::Removed;
            before = runs[earlier].back();
        } else {
            game = int(last.size());
            kind = earlier >= 0 ? BreakKind::Restart : BreakKind::OtherScreen;
            last.push_back(first);
        }
        if (index > 0) {
            const bool is_new = kind == BreakKind::Restart || kind == BreakKind::OtherScreen;
            games.breaks.push_back({before, first, kind, game + 1, is_new ? std::nullopt : std::optional<Fix>(last[game])});
        }
        last[game] = runs[index].back();
        games.of_run.push_back(game + 1);
    }
    return games;
}

}  // namespace jolt
