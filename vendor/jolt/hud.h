#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "video.h"
#include "wobble.h"

namespace jolt {

// video pixels, x y is the leftmost shank's top left at rest, scale is px per gui pixel
struct Bar {
    double x, y, scale;

    Rect rect() const;
    Bar hearts() const;
};

struct Image {
    int width = 0, height = 0, channels = 1;
    std::vector<float> data;

    Image() = default;
    Image(int width, int height, int channels)
        : width(width), height(height), channels(channels), data(size_t(width) * height * channels) {}

    float* at(int x, int y) { return data.data() + (size_t(y) * width + x) * channels; }
    const float* at(int x, int y) const { return data.data() + (size_t(y) * width + x) * channels; }
};

// a jolt with fewer read than this fits too many ticks to be matched alone
inline constexpr int MIN_READ_SHANKS = 8;
// fewer hearts read over a chain than this fit too many ticks to be worth looking up
inline constexpr int MIN_CHAIN_BITS = 20;
// enough ticks to place a chain on one heart, short enough that lag seldom breaks it
inline constexpr double CHAIN_SECONDS = 3;

struct Jolt {
    Row row;
    int frame;
    double time;
    int frames;
    Offsets offsets;
    // food, or health for a heart jolt, -1 if unreadable
    int level;
    int screen = 0;
    // hearts too faint for a jolt of their own are matched a few seconds of ticks at a time, -1 if not
    int chain = -1;
    // ticks after the chain's first jolt
    int link = 0;
    // a bit per heart read too thinly to place the chain, on one frame, outvoted or on a tick missing frames
    std::uint16_t unsure = 0;
};

class BarModel {
public:
    struct Reading {
        int level;
        double presence;
        bool can_jolt;
    };

    BarModel(const Bar& bar, Row row);

    const Rect& rect() const { return rect_; }
    Row row() const { return row_; }
    Reading read(const Image& strip) const;
    bool may_jolt(const std::uint8_t* strip) const;
    bool shows(const std::uint8_t* strip) const;

    struct Rest {
        bool usable = false;
        int level = -1;
        // up, level, down
        std::array<Image, 3> moved;
        std::array<float, SHANKS> contrast{};
        // hearts only, which ones the two rows tell apart
        std::array<bool, SHANKS> apart{};
        // hearts only, the empty ones were too faint to read
        bool dark = false;
    };

    Rest rest_of(const std::uint8_t* strips, int frames) const;

    int measure(const std::uint8_t* strip, const std::array<const Rest*, 3>& rests, std::int8_t* states) const;

private:
    struct Mask {
        std::vector<std::pair<int, float>> cells;
        double total = 0;
    };
    struct Fit {
        // 0 empty, 1 half, 2 full
        int fill;
        double score;
        // spread of the strip under the sprite
        double contrast = 0;
    };
    struct Spread {
        double mean = 0, energy = 0;
    };

    template <typename Pixel>
    Fit fit(const Pixel* strip, int style, int slot, bool lowered) const;
    // hearts take the better of the two rows
    template <typename Pixel>
    std::array<Fit, SHANKS> fits_of(const Pixel* strip, int style) const;
    template <typename Pixel>
    Reading read_pixels(const Pixel* strip) const;
    // which hearts are far fainter than the leftmost, none when that one isn't plainly filled
    static std::optional<std::array<bool, SHANKS>> faint_hearts(const std::array<Fit, SHANKS>& fits);
    // rightmost icon first
    Reading reading_of(const std::array<int, SHANKS>& fills, double presence) const;
    bool rest_food(const std::uint8_t* strips, int frames, Rest& rest) const;
    bool rest_hearts(const std::uint8_t* strips, int frames, Rest& rest) const;
    // for low health too brief to get a resting picture
    int measure_alone(const std::uint8_t* strip, std::int8_t* states) const;
    // hearts blink white for a second after damage or healing, read those by the outline
    std::optional<int> measure_blinking(const std::uint8_t* strip, std::span<const Rest* const> rests,
                                        std::int8_t* states) const;

    Bar bar_;
    Row row_;
    Rect rect_;
    std::array<std::array<Mask, SHANKS>, 3> masks_;
    std::array<std::array<Mask, SHANKS>, 2> outlines_;
    std::vector<std::array<std::array<Image, 3>, 2>> images_;
    std::vector<std::array<std::array<std::array<Spread, SHANKS>, 3>, 2>> spreads_;
};

std::vector<Jolt> find_jolts(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& foods,
                             const std::vector<double>& times);

// low hearts redraw every tick, each run of matching frames is one jolt
std::vector<Jolt> find_heart_jolts(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& healths,
                                   const std::vector<double>& times);

// the frames find_heart_jolts leaves out, cut into chains numbered from first_chain, one jolt per tick
std::vector<Jolt> find_heart_chains(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& healths,
                                    const std::vector<double>& times, const std::vector<Jolt>& heart_jolts,
                                    int first_chain);

std::optional<Bar> locate(const std::vector<Frame>& frames);

Bar find_bar(const std::string& path, const VideoInfo& info, int first, int last);

// main bar first, other players' screens are only searched on mp4 keyframes
std::vector<Bar> find_bars(const std::string& path, const VideoInfo& info, int first, int last, double fps, int jobs);

}  // namespace jolt
