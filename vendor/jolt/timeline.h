#pragma once

#include <array>
#include <optional>
#include <vector>

#include "hud.h"
#include "wobble.h"

namespace jolt {

// at 2 hearts or less each container eats a random draw before the food bar, 10 plus up to 2 absorption
inline constexpr std::array<int, 4> SKIPPED_DRAWS = {0, 10, 11, 12};
inline constexpr std::array<int, 3> HEARTS_ABOVE = {0, 1, 2};

struct Fix {
    int jolt;
    double time;
    int tick;
    int skipped_draws;
    // ticks that fit at least as well, including this one
    int rivals;
    // matched as part of a chain whose first fix stands for it, so no witness of its own
    bool chained = false;

    double uptime_at_video_start() const { return double(tick) / TICKS_PER_SECOND - time; }
};

using Run = std::vector<Fix>;

enum class BreakKind {
    // counter fell behind, lag or a pause
    Stalled,
    // counter ran ahead, footage was cut
    Removed,
    // lower counter, a game started later
    Restart,
    // an earlier game is back
    Back,
    // another player's screen
    OtherScreen,
};

struct Break {
    Fix before, after;
    BreakKind kind;
    // 1 based
    int game;
    // that game's last reading, can be before another screen's run, none for a new game
    std::optional<Fix> last_seen;

    double jump() const { return after.uptime_at_video_start() - last_seen->uptime_at_video_start(); }
};

struct Games {
    // 1 based
    std::vector<int> of_run;
    // one per run after the first
    std::vector<Break> breaks;
};

std::vector<std::optional<Fix>> decode(const std::vector<Jolt>& jolts, const std::vector<WobbleTable>& tables,
                                       double tolerance);

std::vector<Run> confirmed_runs(const std::vector<std::optional<Fix>>& fixes, int max_tick, double tolerance);

std::vector<Run> runs_in_order(const std::vector<std::optional<Fix>>& fixes, const std::vector<Jolt>& jolts,
                               double tolerance);

// a game only ever loses ticks against the clock, so a higher counter that isn't further ahead is the same game
Games sort_into_games(const std::vector<Run>& runs, const std::vector<Jolt>& jolts, double tolerance);

}  // namespace jolt
