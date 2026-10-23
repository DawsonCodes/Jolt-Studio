#pragma once

#include <array>
#include <string>
#include <vector>

#include "hud.h"
#include "timeline.h"

namespace jolt {

struct FramePosition {
    int number = 0, second = 0, within_second = 0;
    double time = 0;
};

// uses frame times, vfr video has no fixed fps
FramePosition frame_position(const std::vector<double>& times, int number);

struct Analysis {
    // main screen first
    std::vector<Bar> bars;
    double fps;
    double span_begin, span_end;
    double readable;
    std::vector<Jolt> jolts;
    std::vector<Run> runs;
    Games games;
    // one pair per break, empty unless it's a cut
    std::vector<std::array<std::string, 2>> frames;
    // where ffmpeg gave nothing
    std::vector<std::array<double, 2>> unread;
    std::vector<std::array<FramePosition, 2>> frame_positions;
};

std::vector<double> launches(const Analysis& analysis);
std::vector<int> screens(const Analysis& analysis);

std::string clock_text(double seconds, int decimals);
// 68, 1:08 or 1:02:03.5
double seconds_of(const std::string& text);

std::string describe(const Analysis& analysis);
std::string verdict(const Analysis& analysis);
std::string to_json(const Analysis& analysis, const std::string& title, const std::string& source);
std::string to_html(const std::string& json);
void save(const std::string& path, const std::string& text);
std::string load(const std::string& path);

}  // namespace jolt
