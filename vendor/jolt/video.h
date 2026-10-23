#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "child.h"

namespace jolt {

struct Rect {
    int left, top, width, height;
};

struct Frame {
    int width = 0, height = 0;
    // bgr rows
    std::vector<std::uint8_t> pixels;
};

struct VideoInfo {
    int width = 0, height = 0;
    std::string codec, format;
    // seconds, first frame at 0
    std::vector<double> times;
    // ffmpeg can't seek to all of these
    std::vector<int> keyframes;
    // spread of pts - dts in seconds
    double reorder = 0;
    // only ffmpeg's mp4 reader can hand over keyframes alone
    bool keyframes_alone = false;
};

struct Download {
    std::string path, title;
};

struct Decoder {
    std::string name = "CPU";
    // empty on the cpu
    std::string method;
    std::string before_input;
    // intel crops the strip on the card
    bool crops_on_card = false;

    bool on_card() const { return !method.empty(); }
};

// ffmpeg crashes now and then
inline constexpr int TRIES = 3;
// seconds of silence before ffmpeg counts as hung
inline constexpr double PATIENCE = 30;

struct Tool {
    const char* name;
    std::vector<const char*> programs;
    const char* stand_in;
    const char* url;
    // empty when the download is the program itself
    const char* unpack;
    int megabytes;
    bool for_links;
    const char* version;
};

const std::vector<Tool>& tools();

// also adds the folder to PATH when it was found somewhere else
std::string where(const char* program);

std::string exe_folder();

std::string version_of(const Tool& tool, const std::string& program, std::string* why = nullptr);

std::vector<const Tool*> missing_tools(bool link);

void fetch_tool(const Tool& tool);

void ready_tools(bool link);

Download fetch_youtube(const std::string& url, const std::string& folder, const std::string& extra_arguments);

VideoInfo probe(const std::string& path);

Frame frame_at(const std::string& path, const VideoInfo& info, double time, std::string* why = nullptr);

std::string jpeg_at(const std::string& path, double time, int width);

// later is frame offsets from the first frame at time, threads 0 leaves it to ffmpeg
std::vector<std::string> jpegs_at(const std::string& path, double time, const std::vector<int>& later, int width,
                                  int threads = 0);

// only the rect gets converted to bgr, way cheaper than whole frames
class StripReader {
public:
    // keyframes_until needs VideoInfo::keyframes_alone, ffmpeg drops some keyframes so frames carry their time
    StripReader(const std::string& path, const Rect& rect, double start, long long frames, const Decoder& decoder = {},
                std::optional<double> keyframes_until = std::nullopt, double patience = PATIENCE);
    StripReader(const std::string& path, const std::vector<Rect>& rects, double start, long long frames,
                const Decoder& decoder = {}, std::optional<double> keyframes_until = std::nullopt,
                double patience = PATIENCE);
    StripReader(const StripReader&) = delete;
    StripReader& operator=(const StripReader&) = delete;

    // fewer than count means ffmpeg stopped
    int read(std::uint8_t* out, int count);

    // off by up to VideoInfo::reorder
    double time() const { return time_; }

    void stop() { ffmpeg_->kill(); }

    std::string close();

private:
    // ffmpeg crops to even corners
    struct Part {
        Rect rect;
        int odd_x, odd_y, wide, tall;
    };

    std::optional<Child> ffmpeg_;
    std::vector<Part> parts_;
    int wide_ = 0, tall_ = 0;
    bool timed_;
    double start_, time_ = 0;
    std::vector<std::uint8_t> frame_;
};

}  // namespace jolt
