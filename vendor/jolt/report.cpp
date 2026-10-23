#include "report.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>

#include "trouble.h"
#include "version.h"
#include "viewer_html.h"

namespace jolt {
namespace {

constexpr const char* DATA_MARK = "/*DATA*/";

// 7.63 s, 4 min 13 s, 2 h 55 min
std::string duration_text(double seconds) {
    if (seconds < 59.995) return std::format("{:.2f} s", seconds);
    const long long rounded = std::llround(seconds), minutes = std::llround(seconds / 60);
    return rounded < 3600 ? std::format("{} min {} s", rounded / 60, rounded % 60)
                          : std::format("{} h {} min", minutes / 60, minutes % 60);
}

std::string break_text(const Break& found) {
    switch (found.kind) {
        case BreakKind::Stalled:
            return std::format("counter fell {} behind: lag or pause", duration_text(-found.jump()));
        case BreakKind::Removed:
            return std::format("COUNTER JUMPED {} AHEAD: footage removed or sped up, or another game instance",
                               duration_text(found.jump()));
        case BreakKind::Restart:
            return std::format("COUNTER WENT BACKWARD: game {}, started later than the one before", found.game);
        case BreakKind::Back:
            return std::format("back to game {}, whose counter fell {} behind since {}", found.game,
                               duration_text(-found.jump()), clock_text(found.last_seen->time, 2));
        case BreakKind::OtherScreen:
            return std::format("another player's screen: game {}, read at a hunger bar that sits elsewhere", found.game);
    }
    return {};
}

const char* name_of(BreakKind kind) {
    switch (kind) {
        case BreakKind::Stalled: return "stalled";
        case BreakKind::Removed: return "removed";
        case BreakKind::Restart: return "restart";
        case BreakKind::Back: return "back";
        case BreakKind::OtherScreen: return "screen";
    }
    return "";
}

// also can't close the script tag it ends up in, utf-8 passes through
std::string json_text(const std::string& text) {
    std::string out = "\"";
    for (unsigned char letter : text) {
        if (letter == '"' || letter == '\\') out += std::format("\\{}", char(letter));
        else if (letter < 0x20 || letter == '<' || letter == 0x7f) out += std::format("\\u{:04x}", letter);
        else out += char(letter);
    }
    return out + "\"";
}

template <typename Value>
std::string or_null(bool present, Value value) {
    return present ? std::format("{}", value) : "null";
}

std::string picture(const std::string& jpeg) {
    constexpr const char* LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out = "data:image/jpeg;base64,";
    for (size_t at = 0; at < jpeg.size(); at += 3) {
        const size_t left = jpeg.size() - at;
        const auto byte = [&](size_t offset) { return offset < left ? unsigned(static_cast<unsigned char>(jpeg[at + offset])) : 0u; };
        const unsigned group = byte(0) << 16 | byte(1) << 8 | byte(2);
        out += LETTERS[group >> 18];
        out += LETTERS[group >> 12 & 63];
        out += left > 1 ? LETTERS[group >> 6 & 63] : '=';
        out += left > 2 ? LETTERS[group & 63] : '=';
    }
    return out;
}

}  // namespace

FramePosition frame_position(const std::vector<double>& times, int number) {
    const double time = times.at(number);
    // subtracting the first pts can leave a boundary a few ulps early
    const int second = int(std::floor(time + 1e-9));
    const auto first = std::lower_bound(times.begin(), times.begin() + number + 1, second - 1e-9);
    return {number, second, number - int(first - times.begin()), time};
}

std::vector<double> launches(const Analysis& analysis) {
    std::vector<double> uptimes;
    for (size_t run = 0; run < analysis.runs.size(); ++run) {
        if (analysis.games.of_run[run] > int(uptimes.size())) {
            uptimes.push_back(analysis.runs[run].front().uptime_at_video_start());
        }
    }
    return uptimes;
}

std::vector<int> screens(const Analysis& analysis) {
    std::vector<int> of_game;
    for (size_t run = 0; run < analysis.runs.size(); ++run) {
        if (analysis.games.of_run[run] > int(of_game.size())) {
            of_game.push_back(analysis.jolts[analysis.runs[run].front().jolt].screen + 1);
        }
    }
    return of_game;
}

std::string clock_text(double seconds, int decimals) {
    if (decimals == 0) seconds = std::round(seconds);
    const int minutes = int(seconds / 60);
    return std::format("{}:{:02}:{:0{}.{}f}", minutes / 60, minutes % 60, seconds - minutes * 60.0,
                       decimals ? decimals + 3 : 2, decimals);
}

double seconds_of(const std::string& text) {
    double total = 0;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t colon = std::min(text.find(':', at), text.size());
        total = total * 60 + std::stod(text.substr(at, colon - at));
        at = colon + 1;
    }
    return total;
}

std::string describe(const Analysis& analysis) {
    std::array<size_t, 2> seen{}, matched{};
    for (const Jolt& jolt : analysis.jolts) ++seen[jolt.row == Row::Hearts];
    for (const Run& run : analysis.runs) {
        for (const Fix& fix : run) ++matched[analysis.jolts[fix.jolt].row == Row::Hearts];
    }
    const bool one_screen = analysis.bars.size() == 1;
    std::string text = std::format("video       {} to {} at {:.2f} fps\n", clock_text(analysis.span_begin, 2),
                                   clock_text(analysis.span_end, 2), analysis.fps);
    for (size_t screen = 0; screen < analysis.bars.size(); ++screen) {
        const Bar& bar = analysis.bars[screen];
        text += std::format("{:<12}x {:.1f}  y {:.1f}  {:.3f} px per gui pixel", screen ? "" : "hunger bar", bar.x, bar.y,
                            bar.scale);
        if (!one_screen) text += std::format(", screen {}", screen + 1);
        if (screen == 0) text += std::format(", readable in {:.0f}% of frames", analysis.readable * 100);
        text += "\n";
    }
    text += std::format("jolts       {} seen, {} matched to a tick\n", seen[0], matched[0]);
    if (seen[1]) text += std::format("heart jolts {} seen, {} matched to a tick\n", seen[1], matched[1]);
    for (const std::array<double, 2>& gap : analysis.unread) {
        text += std::format("NOT READ    {} to {}, ffmpeg gave no frames for it\n", clock_text(gap[0], 2),
                            clock_text(gap[1], 2));
    }
    text += "\n";
    if (analysis.runs.empty()) return text + verdict(analysis) + "\n";

    const std::vector<double> uptimes = launches(analysis);
    const std::vector<int> on_screen = screens(analysis);
    for (size_t game = 0; game < uptimes.size(); ++game) {
        text += std::format("game {}      ", game + 1);
        text += uptimes[game] >= 0
                    ? std::format("had been running {} when the video starts", clock_text(uptimes[game], 2))
                    : std::format("started at {} of the video", clock_text(-uptimes[game], 2));
        text += one_screen ? "\n" : std::format(", on screen {}\n", on_screen[game]);
    }

    text += std::format("\n{:<25}{:<21}{:>5}  game\n", "video time", "ticks", "jolts");
    for (size_t index = 0; index < analysis.runs.size(); ++index) {
        const Fix& first = analysis.runs[index].front();
        const Fix& last = analysis.runs[index].back();
        text += std::format("{} - {:<12}{:<21}{:>5}  {}\n", clock_text(first.time, 2), clock_text(last.time, 2),
                            std::format("{} - {}", first.tick, last.tick), analysis.runs[index].size(),
                            analysis.games.of_run[index]);
        if (index >= analysis.games.breaks.size()) continue;
        const Break& found = analysis.games.breaks[index];
        text += std::format("  between {} and {}: {}\n", clock_text(found.before.time, 2),
                            clock_text(found.after.time, 2), break_text(found));
    }

    text += "\n" + verdict(analysis) + "\n";
    if (uptimes.size() > 1) text += std::format("{} separate game instances appear\n", uptimes.size());
    return text;
}

std::string verdict(const Analysis& analysis) {
    if (analysis.runs.empty()) return "no tick readings, so nothing can be said about cuts";
    double removed = 0;
    int cuts = 0;
    for (const Break& found : analysis.games.breaks) {
        if (found.kind != BreakKind::Removed) continue;
        removed += found.jump();
        ++cuts;
    }
    return cuts ? std::format("at least {} of footage is missing across {} cuts", duration_text(removed), cuts)
                : "no cuts: no game's counter ever ran ahead of the video";
}

std::string to_json(const Analysis& analysis, const std::string& title, const std::string& source) {
    // the jolt that read the video, --render keeps it
    std::string out = std::format(
        "{{\n \"version\": {},\n \"title\": {},\n \"source\": {},\n \"bar\": {{\"x\": {:.3f}, \"y\": {:.3f}, \"scale\": {:.4f}}},\n"
        " \"fps\": {:.4f},\n \"span\": [{:.6f}, {:.6f}],\n \"readable\": {:.4f},\n \"unread\": [",
        json_text(VERSION), json_text(title), json_text(source), analysis.bars[0].x, analysis.bars[0].y,
        analysis.bars[0].scale, analysis.fps, analysis.span_begin, analysis.span_end, analysis.readable);
    for (size_t index = 0; index < analysis.unread.size(); ++index) {
        out += std::format("{}[{:.6f}, {:.6f}]", index ? ", " : "", analysis.unread[index][0], analysis.unread[index][1]);
    }

    // first one is the same as "bar"
    out += "],\n \"screens\": [";
    for (size_t screen = 0; screen < analysis.bars.size(); ++screen) {
        const Bar& bar = analysis.bars[screen];
        out += std::format("{}{{\"x\": {:.3f}, \"y\": {:.3f}, \"scale\": {:.4f}}}", screen ? ", " : "", bar.x, bar.y, bar.scale);
    }

    out += "],\n \"games\": [";
    const std::vector<double> uptimes = launches(analysis);
    const std::vector<int> on_screen = screens(analysis);
    for (size_t game = 0; game < uptimes.size(); ++game) {
        out += std::format("{}{{\"number\": {}, \"uptime_at_video_start\": {:.6f}, \"screen\": {}}}", game ? ", " : "",
                           game + 1, uptimes[game], on_screen[game]);
    }

    out += "],\n \"runs\": [";
    for (size_t index = 0; index < analysis.runs.size(); ++index) {
        out += std::format("{}\n  {{\"game\": {}, \"points\": [", index ? "," : "", analysis.games.of_run[index]);
        const Run& run = analysis.runs[index];
        for (size_t at = 0; at < run.size(); ++at) {
            out += std::format("{}[{:.6f}, {}, {}]", at ? ", " : "", run[at].time, run[at].tick, run[at].jolt);
        }
        out += "]}";
    }

    out += "\n ],\n \"breaks\": [";
    for (size_t index = 0; index < analysis.games.breaks.size(); ++index) {
        const Break& found = analysis.games.breaks[index];
        const bool known = found.last_seen.has_value();
        out += std::format(
            "{}\n  {{\"from\": {:.6f}, \"to\": {:.6f}, \"since\": {}, \"kind\": \"{}\", \"game\": {}, "
            "\"jump_seconds\": {}",
            index ? "," : "", found.before.time, found.after.time,
            known ? std::format("{:.6f}", found.last_seen->time) : "null", name_of(found.kind), found.game,
            known ? std::format("{:.6f}", found.jump()) : "null");
        // only when both frames came through
        if (index < analysis.frames.size() && !analysis.frames[index][0].empty() && !analysis.frames[index][1].empty()) {
            out += ", \"frames\": [\"" + picture(analysis.frames[index][0]) + "\", \"" + picture(analysis.frames[index][1]) + "\"]";
            if (index < analysis.frame_positions.size()) {
                out += ", \"frame_positions\": [";
                for (int side = 0; side < 2; ++side) {
                    const FramePosition& position = analysis.frame_positions[index][side];
                    out += std::format("{}{{\"number\": {}, \"second\": {}, \"within_second\": {}, \"time\": {:.6f}}}",
                                       side ? ", " : "", position.number, position.second,
                                       position.within_second, position.time);
                }
                out += "]";
            }
        }
        out += "}";
    }

    std::vector<const Fix*> fix_of(analysis.jolts.size(), nullptr);
    for (const Run& run : analysis.runs) {
        for (const Fix& fix : run) fix_of[fix.jolt] = &fix;
    }
    out += "\n ],\n \"jolts\": [";
    for (size_t index = 0; index < analysis.jolts.size(); ++index) {
        const Jolt& jolt = analysis.jolts[index];
        out += std::format("{}\n  {{\"time\": {:.6f}, \"frame\": {}, \"frames\": {}, \"offsets\": [", index ? "," : "",
                           jolt.time, jolt.frame, jolt.frames);
        for (int shank = 0; shank < SHANKS; ++shank) {
            out += (shank ? ", " : "") + or_null(jolt.offsets[shank] != UNREAD, int(jolt.offsets[shank]));
        }
        const Fix* fix = fix_of[index];
        // heart jolts get health instead of food
        out += std::format("], \"{}\": {}, \"tick\": {}, \"skipped_draws\": {}}}",
                           jolt.row == Row::Hearts ? "health" : "food", or_null(jolt.level >= 0, jolt.level),
                           or_null(fix != nullptr, fix ? fix->tick : 0),
                           or_null(fix != nullptr, fix ? fix->skipped_draws : 0));
    }
    return out + "\n ]\n}\n";
}

std::string to_html(const std::string& json) {
    std::string page;
    for (const char* piece : VIEWER_HTML) page += piece;
    return page.replace(page.find(DATA_MARK), std::string(DATA_MARK).size(), json);
}

void save(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.flush();
    if (!out) {
        throw Problem(code::CANNOT_WRITE, "Jolt could not write " + path,
                      "The folder may be missing or read only, the disk full, or the file open in a program that "
                      "locks it. Pick another place for the report page and read the video again.",
                      std::format("{} bytes to {}", text.size(), path));
    }
}

std::string load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace jolt
