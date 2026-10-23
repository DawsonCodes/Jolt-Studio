#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numeric>

#include "parallel.h"
#include "sprites.h"
#include "trouble.h"

namespace jolt {
namespace {

constexpr int SPACING = 8;
constexpr int MARGIN = 2;
constexpr int BAR_WIDTH = (SHANKS - 1) * SPACING + sprites::SIZE;
constexpr int CELLS = sprites::SIZE * sprites::SIZE;
constexpr int FILLS = 3;
constexpr int LOOKS = 2 * FILLS;

constexpr int REST_SAMPLES = 24;
constexpr double MIN_PRESENCE = 0.6;
constexpr double MIN_FRAME_PRESENCE = 0.5;
// empty hearts over bright ground only score ~0.3
constexpr double MIN_HEARTS_PRESENCE = 0.3;
constexpr double MIN_CLEAR_PRESENCE = 0.6;
// appleskin gold measured ~182,157,58, plain outline ~30,15,15
constexpr double GOLD_MIN_RED = 110;
constexpr double GOLD_MIN_GREEN = 90;
constexpr double GOLD_MAX_BLUE_SHARE = 0.6;
constexpr double MIN_STD = 4.0;
constexpr float MIN_CONTRAST = 6.0f;
constexpr float MAX_COST = 0.5f;
// the ground moving behind the red heart's edges in the dark keeps frames further from its resting picture, the
// ratio still tells its rows apart
constexpr float MAX_DARK_COST = 0.8f;
constexpr float MAX_RATIO = 0.6f;

constexpr int HEARTS_LEFT = 101;
constexpr int LOW_HEARTS = 2;
constexpr double ROW_MARGIN = 0.1;
constexpr int MIN_ROW_FRAMES = 4;
constexpr int HEART_SAMPLES = 6;
// an open menu dims the hud to about a quarter
constexpr float MAX_ROW_GAP = 0.25f;
constexpr int MIN_READ_HEARTS = 7;
// darkest channel, blinking outline ~250, black ~55, stays under 170 even over snow
constexpr float BLINK_WHITE = 200;
constexpr int BLINKING_HEARTS = 7;
// a heart's rows must differ like the sprite moved, sharp footage stays under 0.6, faint hearts in the dark
// differ by what moved behind them and mostly score 0.7 to 1.3
constexpr float MAX_SHIFT_MISMATCH = 0.6f;
// empty hearts the dark has smeared away keep 0.1 to 0.3 of the red heart's contrast, a half heart 0.75
constexpr double MAX_FAINT_SHARE = 0.5;
constexpr double MAX_CHAIN_GAP = 0.5;
// guesses at where ticks begin between frames, enough for 120 fps
constexpr int PHASES = 6;

// real footage scores ~0.7, a lone false match can beat that so frames have to agree
constexpr double MIN_ROW_SCORE = 0.6;
constexpr int MIN_VOTES = 3;
constexpr int MIN_SIZE = 13;
constexpr int MAX_SIZE = 81;
constexpr int FINE = 18;
constexpr int COARSE = 9;
constexpr int PROPOSALS = 3;
constexpr int SEARCH_THREADS = 12;
constexpr int SAMPLE_FRAMES = 12;
constexpr int OTHER_TRIES = 6;
constexpr int FOLLOWED_KEYFRAMES = 4;
constexpr double LATER[] = {0.15, 0.3, 0.45};

struct Icon {
    std::array<float, CELLS * 3> bgr;
    std::array<float, CELLS> grey;
    std::array<bool, CELLS> opaque;
};

Icon compose(const sprites::Pixels& base, const sprites::Pixels* top) {
    Icon icon{};
    for (int cell = 0; cell < CELLS; ++cell) {
        const bool covered = top && (*top)[cell * 4 + 3];
        const std::uint8_t* pixel = (covered ? top->data() : base.data()) + cell * 4;
        icon.opaque[cell] = covered || base[cell * 4 + 3];
        for (int channel = 0; channel < 3; ++channel) icon.bgr[cell * 3 + channel] = pixel[channel];
        icon.grey[cell] = 0.114f * pixel[0] + 0.587f * pixel[1] + 0.299f * pixel[2];
    }
    return icon;
}

int styles(Row row) { return row == Row::Food ? 2 : 1; }

const Icon& icon(Row row, int style, int fill) {
    using namespace sprites;
    static const std::array<std::array<Icon, FILLS>, 3> icons = {{
        {compose(FOOD_EMPTY, nullptr), compose(FOOD_EMPTY, &FOOD_HALF), compose(FOOD_EMPTY, &FOOD_FULL)},
        {compose(FOOD_EMPTY_HUNGER, nullptr), compose(FOOD_EMPTY_HUNGER, &FOOD_HALF_HUNGER),
         compose(FOOD_EMPTY_HUNGER, &FOOD_FULL_HUNGER)},
        {compose(HEART_CONTAINER, nullptr), compose(HEART_CONTAINER, &HEART_HALF),
         compose(HEART_CONTAINER, &HEART_FULL)},
    }};
    return icons[row == Row::Food ? style : 2][fill];
}

// exact area coverage per pixel, so non integer scales come out right
void paint(const Icon& icon, const float* values, double x0, double y0, double scale, Image& coverage,
           Image* painted) {
    for (int cell = 0; cell < CELLS; ++cell) {
        if (!icon.opaque[cell]) continue;
        const double left = x0 + cell % sprites::SIZE * scale, right = left + scale;
        const double top = y0 + cell / sprites::SIZE * scale, bottom = top + scale;
        const int last_x = std::min(coverage.width, int(std::ceil(right)));
        const int last_y = std::min(coverage.height, int(std::ceil(bottom)));
        for (int y = std::max(0, int(std::floor(top))); y < last_y; ++y) {
            const double tall = std::min(bottom, y + 1.0) - std::max(top, double(y));
            for (int x = std::max(0, int(std::floor(left))); x < last_x; ++x) {
                const float area = float(tall * (std::min(right, x + 1.0) - std::max(left, double(x))));
                *coverage.at(x, y) += area;
                if (!painted) continue;
                for (int channel = 0; channel < painted->channels; ++channel) {
                    painted->at(x, y)[channel] += area * values[cell * painted->channels + channel];
                }
            }
        }
    }
}

void divide_by_coverage(Image& painted, const Image& coverage) {
    for (size_t pixel = 0; pixel < coverage.data.size(); ++pixel) {
        const float share = std::max(coverage.data[pixel], 1e-3f);
        for (int channel = 0; channel < painted.channels; ++channel) {
            painted.data[pixel * painted.channels + channel] /= share;
        }
    }
}

float median(std::vector<float>& values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2) return *middle;
    return (*middle + *std::max_element(values.begin(), middle)) / 2;
}

// rank select a bit at a time across all columns, way faster than sorting each one
void ranked(const std::vector<const std::uint8_t*>& samples, const size_t count, const size_t rank,
            std::uint8_t* __restrict found) {
    // restrict or the compiler assumes aliasing and goes byte by byte
    std::vector<std::uint8_t> trials(count), counted(count);
    std::vector<std::uint32_t> belows(count);
    std::uint8_t* __restrict trial = trials.data();
    std::uint8_t* __restrict some = counted.data();
    std::uint32_t* __restrict below = belows.data();
    std::fill_n(found, count, std::uint8_t(0));
    for (int bit = 128; bit; bit >>= 1) {
        for (size_t value = 0; value < count; ++value) trial[value] = std::uint8_t(found[value] | bit);
        std::fill_n(below, count, 0u);
        // byte counters, flushed before they can overflow
        for (size_t first = 0; first < samples.size(); first += 255) {
            std::fill_n(some, count, std::uint8_t(0));
            const size_t last = std::min(first + 255, samples.size());
            for (size_t sample = first; sample < last; ++sample) {
                const std::uint8_t* __restrict row = samples[sample];
                for (size_t value = 0; value < count; ++value) some[value] += row[value] < trial[value];
            }
            for (size_t value = 0; value < count; ++value) below[value] += some[value];
        }
        for (size_t value = 0; value < count; ++value) {
            found[value] = below[value] <= rank ? trial[value] : found[value];
        }
    }
}

void medians(const std::vector<const std::uint8_t*>& samples, const size_t count, float* out) {
    std::vector<std::uint8_t> upper(count), lower(count);
    ranked(samples, count, samples.size() / 2, upper.data());
    if (samples.size() % 2) {
        std::copy(upper.begin(), upper.end(), out);
        return;
    }
    ranked(samples, count, samples.size() / 2 - 1, lower.data());
    for (size_t value = 0; value < count; ++value) out[value] = (float(upper[value]) + float(lower[value])) / 2;
}

Image moved_down(const Image& image, double rows) {
    Image out(image.width, image.height, image.channels);
    const int row = image.width * image.channels;
    for (int y = 0; y < image.height; ++y) {
        const double from = y - rows;
        const double floor = std::floor(from);
        const float blend = float(from - floor);
        const float* upper = image.at(0, std::clamp(int(floor), 0, image.height - 1));
        const float* lower = image.at(0, std::clamp(int(floor) + 1, 0, image.height - 1));
        float* to = out.at(0, y);
        for (int value = 0; value < row; ++value) to[value] = upper[value] + blend * (lower[value] - upper[value]);
    }
    return out;
}

}  // namespace

Rect Bar::rect() const {
    return {
        int(std::floor(x - MARGIN * scale)),
        int(std::floor(y - MARGIN * scale)),
        int(std::ceil((BAR_WIDTH + 2 * MARGIN) * scale)) + 1,
        int(std::ceil((sprites::SIZE + 2 * MARGIN) * scale)) + 1,
    };
}

Bar Bar::hearts() const { return {x - HEARTS_LEFT * scale, y, scale}; }

BarModel::BarModel(const Bar& bar, Row row) : bar_(bar), row_(row), rect_(bar.rect()) {
    const double y0 = bar.y - rect_.top;
    const auto shank_x = [&](int slot) { return bar.x - rect_.left + (SHANKS - 1 - slot) * SPACING * bar.scale; };

    const auto mask_of = [&](const Icon& drawn, int slot, int rows) {
        Image coverage(rect_.width, rect_.height, 1);
        paint(drawn, nullptr, shank_x(slot), y0 + rows * bar.scale, bar.scale, coverage, nullptr);
        Mask mask;
        for (int pixel = 0; pixel < int(coverage.data.size()); ++pixel) {
            // squared so half covered edge pixels barely count
            const float weight = coverage.data[pixel] * coverage.data[pixel];
            if (weight <= 0) continue;
            mask.cells.emplace_back(pixel, weight);
            mask.total += weight;
        }
        return mask;
    };
    Icon outline = icon(row, 0, 0);
    for (int cell = 0; cell < CELLS; ++cell) outline.opaque[cell] = outline.opaque[cell] && outline.grey[cell] == 0;
    for (int slot = 0; slot < SHANKS; ++slot) {
        for (int offset = 0; offset < 3; ++offset) masks_[offset][slot] = mask_of(icon(row, 0, 0), slot, offset - 1);
        for (int lowered = 0; lowered < 2; ++lowered) outlines_[lowered][slot] = mask_of(outline, slot, lowered);
    }

    images_.resize(styles(row));
    spreads_.resize(styles(row));
    for (int style = 0; style < styles(row); ++style) {
        for (int lowered = 0; lowered < (row == Row::Hearts ? 2 : 1); ++lowered) {
            for (int fill = 0; fill < FILLS; ++fill) {
                Image coverage(rect_.width, rect_.height, 1);
                Image& image = images_[style][lowered][fill] = Image(rect_.width, rect_.height, 3);
                const Icon& drawn = icon(row, style, fill);
                for (int slot = 0; slot < SHANKS; ++slot) {
                    paint(drawn, drawn.bgr.data(), shank_x(slot), y0 + lowered * bar.scale, bar.scale, coverage, &image);
                }
                divide_by_coverage(image, coverage);
                for (int slot = 0; slot < SHANKS; ++slot) {
                    const Mask& mask = masks_[lowered ? 2 : 1][slot];
                    Spread& spread = spreads_[style][lowered][fill][slot];
                    double sum = 0;
                    for (const auto& [pixel, weight] : mask.cells) {
                        for (int channel = 0; channel < 3; ++channel) sum += weight * image.data[pixel * 3 + channel];
                    }
                    spread.mean = sum / (mask.total * 3);
                    for (const auto& [pixel, weight] : mask.cells) {
                        for (int channel = 0; channel < 3; ++channel) {
                            const double away = image.data[pixel * 3 + channel] - spread.mean;
                            spread.energy += weight * away * away;
                        }
                    }
                }
            }
        }
    }
}

template <typename Pixel>
BarModel::Fit BarModel::fit(const Pixel* strip, int style, int slot, bool lowered) const {
    const Mask& mask = masks_[lowered ? 2 : 1][slot];
    const std::array<Image, FILLS>& images = images_[style][lowered];
    const std::array<std::array<Spread, SHANKS>, FILLS>& spreads = spreads_[style][lowered];
    const double total = mask.total * 3;
    double sum = 0;
    for (const auto& [pixel, weight] : mask.cells) {
        for (int channel = 0; channel < 3; ++channel) sum += weight * strip[pixel * 3 + channel];
    }
    const double mean = sum / total;
    double energy = 0;
    std::array<double, FILLS> shared{};
    for (const auto& [pixel, weight] : mask.cells) {
        for (int channel = 0; channel < 3; ++channel) {
            const int value = pixel * 3 + channel;
            const double away = strip[value] - mean;
            energy += weight * away * away;
            for (int fill = 0; fill < FILLS; ++fill) {
                shared[fill] += weight * away * (images[fill].data[value] - spreads[fill][slot].mean);
            }
        }
    }
    const double floor = total * MIN_STD * MIN_STD;
    Fit best{0, -2, std::sqrt(energy / total)};
    for (int fill = 0; fill < FILLS; ++fill) {
        const double score = shared[fill] / std::sqrt(std::max(energy * spreads[fill][slot].energy, floor * floor));
        if (score > best.score) {
            best.fill = fill;
            best.score = score;
        }
    }
    return best;
}

template <typename Pixel>
std::array<BarModel::Fit, SHANKS> BarModel::fits_of(const Pixel* strip, int style) const {
    std::array<Fit, SHANKS> found;
    for (int slot = 0; slot < SHANKS; ++slot) {
        found[slot] = fit(strip, style, slot, false);
        if (row_ == Row::Hearts) {
            const Fit low = fit(strip, style, slot, true);
            if (low.score > found[slot].score) found[slot] = low;
        }
    }
    return found;
}

template <typename Pixel>
BarModel::Reading BarModel::read_pixels(const Pixel* strip) const {
    std::array<int, SHANKS> fills{};
    double presence = -2;
    for (int style = 0; style < int(images_.size()); ++style) {
        const std::array<Fit, SHANKS> found = fits_of(strip, style);
        double mean = 0;
        for (const Fit& one : found) mean += one.score / SHANKS;
        if (mean > presence) {
            presence = mean;
            for (int slot = 0; slot < SHANKS; ++slot) fills[slot] = found[slot].fill;
        }
    }

    return reading_of(fills, presence);
}

// the dark can smear the empty hearts into the ground while the red one stays plain to see
std::optional<std::array<bool, SHANKS>> BarModel::faint_hearts(const std::array<Fit, SHANKS>& fits) {
    const Fit& leftmost = fits[SHANKS - 1];
    if (leftmost.fill == 0 || leftmost.score < MIN_CLEAR_PRESENCE) return std::nullopt;
    std::array<bool, SHANKS> faint;
    for (int slot = 0; slot < SHANKS; ++slot) faint[slot] = fits[slot].contrast < MAX_FAINT_SHARE * leftmost.contrast;
    return faint;
}

BarModel::Reading BarModel::read(const Image& strip) const { return read_pixels(strip.data.data()); }

BarModel::Reading BarModel::reading_of(const std::array<int, SHANKS>& fills, double presence) const {
    int level = 0;
    for (int fill : fills) level += fill;
    for (int slot = 0; slot < SHANKS; ++slot) {
        // food drains from the left, the highest index, health from the right
        const int place = row_ == Row::Food ? slot : SHANKS - 1 - slot;
        if (fills[slot] != std::clamp(level - 2 * place, 0, 2)) {
            level = -1;
            break;
        }
    }
    if (row_ == Row::Food) return {level, presence, level != 0};
    return {level, presence, std::all_of(fills.begin(), fills.end() - LOW_HEARTS, [](int fill) { return fill == 0; })};
}

bool BarModel::shows(const std::uint8_t* strip) const {
    return read_pixels(strip).presence >= (row_ == Row::Hearts ? MIN_HEARTS_PRESENCE : MIN_FRAME_PRESENCE);
}

bool BarModel::may_jolt(const std::uint8_t* strip) const {
    const Reading reading = read_pixels(strip);
    if (row_ == Row::Hearts) {
        if (reading.presence >= MIN_HEARTS_PRESENCE && reading.can_jolt) return true;
        const std::array<Fit, SHANKS> found = fits_of(strip, 0);
        const std::optional<std::array<bool, SHANKS>> faint = faint_hearts(found);
        if (!faint) return false;
        std::array<int, SHANKS> fills;
        for (int slot = 0; slot < SHANKS; ++slot) fills[slot] = (*faint)[slot] ? 0 : found[slot].fill;
        return reading_of(fills, reading.presence).can_jolt;
    }
    if (reading.presence < MIN_FRAME_PRESENCE || !reading.can_jolt) return false;

    // appleskin outlines shanks in gold while there's saturation, no jolts then
    for (const Mask& outline : outlines_[0]) {
        std::array<double, 3> bgr{};
        for (const auto& [pixel, weight] : outline.cells) {
            for (int channel = 0; channel < 3; ++channel) bgr[channel] += weight * strip[pixel * 3 + channel];
        }
        const double red = bgr[2] / outline.total, green = bgr[1] / outline.total, blue = bgr[0] / outline.total;
        if (red > GOLD_MIN_RED && green > GOLD_MIN_GREEN && blue < GOLD_MAX_BLUE_SHARE * red) return false;
    }
    return true;
}

namespace {

template <typename Mask, typename Pixel>
float mismatch(const Mask& mask, const Pixel* pixels, const Image& against) {
    double sum = 0;
    for (const auto& [pixel, weight] : mask.cells) {
        float difference = 0;
        for (int channel = 0; channel < 3; ++channel) {
            difference += std::abs(float(pixels[pixel * 3 + channel]) - against.data[pixel * 3 + channel]);
        }
        sum += weight * difference;
    }
    return float(sum / mask.total);
}

template <int Count, typename Mask>
std::array<float, 3> mismatches(const Mask& mask, const std::uint8_t* pixels,
                                const std::array<const float*, 3>& against) {
    std::array<double, Count> sums{};
    for (const auto& [pixel, weight] : mask.cells) {
        const std::uint8_t* seen = pixels + pixel * 3;
        const float blue = seen[0], green = seen[1], red = seen[2];
        for (int image = 0; image < Count; ++image) {
            const float* expected = against[image] + pixel * 3;
            const float difference =
                std::abs(blue - expected[0]) + std::abs(green - expected[1]) + std::abs(red - expected[2]);
            sums[image] += weight * difference;
        }
    }
    std::array<float, 3> found{};
    for (int image = 0; image < Count; ++image) found[image] = float(sums[image] / mask.total);
    return found;
}

template <typename Mask>
float brightness(const Mask& mask, const Image& image) {
    double sum = 0;
    for (const auto& [pixel, weight] : mask.cells) {
        for (int channel = 0; channel < 3; ++channel) sum += weight * image.data[pixel * 3 + channel];
    }
    return float(sum / mask.total);
}

template <typename Mask>
float whiteness(const Mask& mask, const std::uint8_t* strip) {
    double sum = 0;
    for (const auto& [pixel, weight] : mask.cells) {
        const std::uint8_t* bgr = strip + pixel * 3;
        sum += weight * std::min(bgr[0], std::min(bgr[1], bgr[2]));
    }
    return float(sum / mask.total);
}

}  // namespace

bool BarModel::rest_hearts(const std::uint8_t* strips, int frames, Rest& rest) const {
    const size_t values = size_t(rect_.width) * rect_.height * 3;
    bool low = false;
    for (int frame = 0; frame < frames && !low; frame += std::max(1, frames / HEART_SAMPLES)) {
        low = may_jolt(strips + frame * values);
    }
    if (!low) return false;

    // low hearts are down half the ticks so the median blends both rows, sort frames by sprite first
    std::array<std::array<std::array<std::vector<int>, FILLS>, SHANKS>, 2> shown, leaning;
    std::array<int, SHANKS> fainter{};
    for (int frame = 0; frame < frames; ++frame) {
        const std::uint8_t* strip = strips + frame * values;
        std::array<Fit, SHANKS> better;
        for (int slot = 0; slot < SHANKS; ++slot) {
            const Fit on_row = fit(strip, 0, slot, false), lowered = fit(strip, 0, slot, true);
            better[slot] = lowered.score > on_row.score ? lowered : on_row;
            leaning[lowered.score > on_row.score][slot][better[slot].fill].push_back(frame);
            if (std::abs(on_row.score - lowered.score) <= ROW_MARGIN) continue;
            shown[lowered.score > on_row.score][slot][better[slot].fill].push_back(frame);
        }
        if (const std::optional<std::array<bool, SHANKS>> faint = faint_hearts(better)) {
            for (int slot = 0; slot < SHANKS; ++slot) fainter[slot] += (*faint)[slot];
        }
    }
    std::array<int, SHANKS> fills{};
    for (int slot = 0; slot < SHANKS; ++slot) {
        const auto frames_with = [&](int fill) { return shown[0][slot][fill].size() + shown[1][slot][fill].size(); };
        for (int fill = 1; fill < FILLS; ++fill) {
            if (frames_with(fill) > frames_with(fills[slot])) fills[slot] = fill;
        }
    }
    Reading reading = reading_of(fills, 1);
    // hearts mostly far fainter than the red one are empty, there's nothing in them to read
    const auto fainter_than_red = [&](int slot) { return fainter[slot] * 2 > frames; };
    std::array<bool, SHANKS> faint{};
    if (!reading.can_jolt) {
        for (int slot = 0; slot < SHANKS; ++slot) {
            faint[slot] = fainter_than_red(slot);
            if (faint[slot]) fills[slot] = 0;
        }
        reading = reading_of(fills, 1);
        if (!reading.can_jolt) return false;
    }
    bool dark = std::find(faint.begin(), faint.end(), true) != faint.end();

    std::vector<int> slot_of(rect_.width);
    for (int x = 0; x < rect_.width; ++x) {
        const double gui = (x + 0.5 - (bar_.x - rect_.left)) / bar_.scale;
        slot_of[x] = SHANKS - 1 - std::clamp(int(std::floor((gui - 0.5) / SPACING)), 0, SHANKS - 1);
    }
    std::vector<int> every(frames);
    std::iota(every.begin(), every.end(), 0);
    // a faint heart's rows are both the whole chunk, as it can't be told which it's on. The red heart sits on the xp
    // bar's dark edge, so on its row the lowered sprite fits nearly as well and in the dark the margin leaves out most
    // of those frames, but a heart this plain is on whichever row fits better
    const auto frames_on = [&](int lowered, int slot) -> const std::vector<int>& {
        if (faint[slot]) return every;
        return (dark && fills[slot] ? leaning : shown)[lowered][slot][fills[slot]];
    };
    const auto seen_on = [&](int lowered, int slot) { return int(frames_on(lowered, slot).size()) >= MIN_ROW_FRAMES; };
    for (int slot = 0; slot < SHANKS; ++slot) {
        if (seen_on(0, slot) || seen_on(1, slot)) continue;
        // so is an empty one the fit puts on neither row
        if (fills[slot] || !fainter_than_red(slot)) return false;
        faint[slot] = dark = true;
    }

    // empty hearts are too faint to tell from a shifted copy, so both rows come from footage
    std::array<Image, 2> rows;
    std::vector<std::uint8_t> gathered;
    std::vector<const std::uint8_t*> samples;
    std::vector<float> middles;
    const auto median_into = [&](const std::vector<int>& from, int left, int right, Image& into) {
        const size_t across = size_t(right - left) * 3, count = across * rect_.height;
        gathered.resize(from.size() * count);
        samples.clear();
        for (size_t index = 0; index < from.size(); ++index) {
            std::uint8_t* to = gathered.data() + index * count;
            const std::uint8_t* frame = strips + from[index] * values;
            for (int y = 0; y < rect_.height; ++y) {
                std::copy_n(frame + (size_t(y) * rect_.width + left) * 3, across, to + y * across);
            }
            samples.push_back(to);
        }
        middles.resize(count);
        medians(samples, count, middles.data());
        for (int y = 0; y < rect_.height; ++y) std::copy_n(middles.data() + y * across, across, into.at(left, y));
    };
    const auto for_each_slot = [&](auto visit) {
        for (int left = 0, right; left < rect_.width; left = right) {
            for (right = left + 1; right < rect_.width && slot_of[right] == slot_of[left]; ++right) {}
            visit(slot_of[left], left, right);
        }
    };
    for (int lowered = 0; lowered < 2; ++lowered) {
        rows[lowered] = Image(rect_.width, rect_.height, 3);
        for_each_slot([&](int slot, int left, int right) {
            if (seen_on(lowered, slot)) median_into(frames_on(lowered, slot), left, right, rows[lowered]);
        });
    }

    const std::array<Image, 2> stand_in = {moved_down(rows[1], -bar_.scale), moved_down(rows[0], bar_.scale)};
    for (int lowered = 0; lowered < 2; ++lowered) {
        for (int x = 0; x < rect_.width; ++x) {
            if (seen_on(lowered, slot_of[x])) continue;
            for (int y = 0; y < rect_.height; ++y) std::copy_n(stand_in[lowered].at(x, y), 3, rows[lowered].at(x, y));
        }
    }
    // menu dimming part of a chunk makes every frame match whichever row has its brightness
    for (int slot = 0; slot < SHANKS; ++slot) {
        if (faint[slot]) continue;
        const float on_row = brightness(masks_[1][slot], rows[0]), lowered = brightness(masks_[2][slot], rows[1]);
        if (std::abs(on_row - lowered) > MAX_ROW_GAP * std::max(on_row, lowered)) return false;
    }
    // a row seen too rarely is checked against the frames that aren't on the other row
    std::array<Image, 2> checked = rows;
    std::array<bool, SHANKS> pictured;
    for (int slot = 0; slot < SHANKS; ++slot) pictured[slot] = !faint[slot];
    std::vector<int> elsewhere;
    for (int lowered = 0; lowered < 2; ++lowered) {
        for_each_slot([&](int slot, int left, int right) {
            if (seen_on(lowered, slot)) return;
            const std::vector<int>& other = frames_on(1 - lowered, slot);
            elsewhere.clear();
            for (int frame = 0; frame < frames; ++frame) {
                if (!std::binary_search(other.begin(), other.end(), frame)) elsewhere.push_back(frame);
            }
            if (int(elsewhere.size()) < MIN_ROW_FRAMES) pictured[slot] = false;
            else median_into(elsewhere, left, right, checked[lowered]);
        });
    }
    const std::array<Image, 2> moved_over = {moved_down(checked[1], -bar_.scale), moved_down(checked[0], bar_.scale)};
    for (int slot = 0; slot < SHANKS; ++slot) {
        const float apart = mismatch(masks_[1][slot], checked[0].data.data(), checked[1]);
        const float off = 0.5f * (mismatch(masks_[1][slot], checked[0].data.data(), moved_over[0]) +
                                  mismatch(masks_[2][slot], checked[1].data.data(), moved_over[1]));
        // in the dark the leftmost heart was plain in most frames, what moves behind it can't pass for it
        rest.apart[slot] = pictured[slot] && (off < MAX_SHIFT_MISMATCH * apart || (dark && slot == SHANKS - 1));
    }
    rest.moved = {moved_down(rows[0], -bar_.scale), rows[0], rows[1]};
    rest.level = reading.level;
    rest.dark = dark;
    return true;
}

bool BarModel::rest_food(const std::uint8_t* strips, int frames, Rest& rest) const {
    const int values = rect_.width * rect_.height * 3;
    // jolts are rare so the median is the bar at rest
    Image still(rect_.width, rect_.height, 3);
    const int step = std::max(1, frames / REST_SAMPLES);
    std::vector<const std::uint8_t*> samples;
    for (int frame = 0; frame < frames; frame += step) samples.push_back(strips + size_t(frame) * values);
    medians(samples, values, still.data.data());

    const Reading reading = read(still);
    // food 0 jolts every tick so there's no rest, dark scenes also read as 10 empty shanks
    if (reading.presence < MIN_PRESENCE || !reading.can_jolt) return false;
    rest.moved = {moved_down(still, -bar_.scale), still, moved_down(still, bar_.scale)};
    rest.level = reading.level;
    return true;
}

BarModel::Rest BarModel::rest_of(const std::uint8_t* strips, int frames) const {
    Rest rest;
    if (!(row_ == Row::Hearts ? rest_hearts(strips, frames, rest) : rest_food(strips, frames, rest))) return rest;

    rest.usable = true;
    for (int slot = 0; slot < SHANKS; ++slot) {
        const Mask& mask = masks_[1][slot];
        const float* still = rest.moved[1].data.data();
        rest.contrast[slot] = std::max(MIN_CONTRAST, 0.5f * (mismatch(mask, still, rest.moved[0]) +
                                                              mismatch(mask, still, rest.moved[2])));
    }
    return rest;
}

int BarModel::measure_alone(const std::uint8_t* strip, std::int8_t* states) const {
    std::fill_n(states, SHANKS, UNREAD);
    std::array<int, SHANKS> fills{};
    Offsets rows;
    rows.fill(UNREAD);
    double presence = 0;
    for (int index = 0; index < SHANKS; ++index) {
        // check the 8 that must be empty first, almost every frame fails there
        const int slot = index < SHANKS - LOW_HEARTS ? SHANKS - LOW_HEARTS - 1 - index : index;
        const Fit on_row = fit(strip, 0, slot, false), lowered = fit(strip, 0, slot, true);
        const Fit& better = lowered.score > on_row.score ? lowered : on_row;
        if (slot < SHANKS - LOW_HEARTS && better.fill != 0) return -1;
        fills[slot] = better.fill;
        presence += better.score / SHANKS;
        if (std::abs(on_row.score - lowered.score) > ROW_MARGIN) rows[slot] = &better == &lowered;
    }
    const Reading reading = reading_of(fills, presence);
    if (presence < MIN_CLEAR_PRESENCE || reading.level < 0) return -1;
    std::copy(rows.begin(), rows.end(), states);
    return reading.level;
}

std::optional<int> BarModel::measure_blinking(const std::uint8_t* strip, std::span<const Rest* const> rests,
                                              std::int8_t* states) const {
    std::array<std::array<float, 2>, SHANKS> lit;
    int white = 0;
    for (int slot = 0; slot < SHANKS; ++slot) {
        for (int lowered = 0; lowered < 2; ++lowered) lit[slot][lowered] = whiteness(outlines_[lowered][slot], strip);
        white += std::max(lit[slot][0], lit[slot][1]) >= BLINK_WHITE;
        if (slot + 1 - white > SHANKS - BLINKING_HEARTS) return std::nullopt;
    }

    // a heart that just lost health is pale inside and too bright to read, leave it
    for (int slot = 0; slot < SHANKS; ++slot) {
        const auto [on_row, down] = lit[slot];
        states[slot] = UNREAD;
        if (std::max(on_row, down) < BLINK_WHITE) continue;
        if (down < MAX_RATIO * on_row) states[slot] = 0;
        else if (on_row < MAX_RATIO * down) states[slot] = 1;
    }
    for (const int at : {1, 0, 2}) {
        if (at < int(rests.size()) && rests[at] && rests[at]->usable) return rests[at]->level;
    }
    return -1;
}

int BarModel::measure(const std::uint8_t* strip, const std::array<const Rest*, 3>& rests, std::int8_t* states) const {
    if (row_ == Row::Hearts) {
        if (const std::optional<int> health = measure_blinking(strip, rests, states)) return *health;
    }
    // low health can be over before any chunk gets a rest of it, and one from the dark reads only the red hearts
    const auto rested = [&](bool counting_dark) {
        return std::any_of(rests.begin(), rests.end(), [&](const Rest* rest) {
            return rest && rest->usable && (counting_dark || !rest->dark);
        });
    };
    if (row_ == Row::Hearts && !rested(false)) {
        const int alone = measure_alone(strip, states);
        if (alone >= 0 || !rested(true)) return alone;
    }

    std::array<const Rest*, 3> usable{};
    int count = 0;
    for (const Rest* rest : rests) {
        if (rest && rest->usable) usable[count++] = rest;
    }
    std::array<std::array<std::array<float, 3>, SHANKS>, 3> mismatched;
    for (int slot = 0; slot < SHANKS && count; ++slot) {
        for (int offset = 0; offset < 3; ++offset) {
            std::array<const float*, 3> against{};
            for (int index = 0; index < count; ++index) against[index] = usable[index]->moved[offset].data.data();
            const Mask& mask = masks_[offset][slot];
            const std::array<float, 3> found = count == 3   ? mismatches<3>(mask, strip, against)
                                               : count == 2 ? mismatches<2>(mask, strip, against)
                                                            : mismatches<1>(mask, strip, against);
            for (int index = 0; index < count; ++index) {
                mismatched[index][slot][offset] = found[index] / usable[index]->contrast[slot];
            }
        }
    }

    std::array<std::array<float, 3>, SHANKS> costs;
    std::array<float, SHANKS> lowest, most;
    lowest.fill(std::numeric_limits<float>::max());
    most.fill(MAX_COST);
    int level = -1;
    float closest = std::numeric_limits<float>::max();

    for (int index = 0; index < count; ++index) {
        const Rest* rest = usable[index];
        float total = 0;
        for (int slot = 0; slot < SHANKS; ++slot) {
            const std::array<float, 3>& cost = mismatched[index][slot];
            const float best = *std::min_element(cost.begin(), cost.end());
            total += best;
            if (best < lowest[slot] && (row_ == Row::Food || rest->apart[slot])) {
                lowest[slot] = best;
                costs[slot] = cost;
                most[slot] = rest->dark ? MAX_DARK_COST : MAX_COST;
            }
        }
        if (total < closest) {
            closest = total;
            level = rest->level;
        }
    }

    for (int slot = 0; slot < SHANKS; ++slot) {
        states[slot] = UNREAD;
        if (lowest[slot] >= most[slot]) continue;
        const std::array<float, 3>& cost = costs[slot];
        const int best = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
        // regen lifts a heart above its row, which hides its offset
        if (row_ == Row::Hearts && best == 0) continue;
        if (cost[best] < MAX_RATIO * std::min(cost[(best + 1) % 3], cost[(best + 2) % 3])) {
            states[slot] = std::int8_t(best - 1);
        }
    }
    return level;
}

std::vector<Jolt> find_jolts(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& foods,
                             const std::vector<double>& times) {
    const int frames = int(foods.size());
    const auto moving = [&](int frame) {
        for (int slot = 0; slot < SHANKS; ++slot) {
            const std::int8_t state = states[size_t(frame) * SHANKS + slot];
            if (state == 1 || state == -1) return true;
        }
        return false;
    };

    std::vector<Jolt> jolts;
    for (int start = 0; start < frames; ++start) {
        if (!moving(start)) continue;
        int stop = start + 1;
        while (stop < frames && moving(stop)) ++stop;

        Jolt jolt{Row::Food, start, times[start], stop - start, {}, foods[start]};
        for (int slot = 0; slot < SHANKS; ++slot) {
            std::array<int, 3> votes{};
            int known = 0;
            for (int frame = start; frame < stop; ++frame) {
                const std::int8_t state = states[size_t(frame) * SHANKS + slot];
                if (state == UNREAD) continue;
                ++votes[state + 1];
                ++known;
            }
            const int winner = int(std::max_element(votes.begin(), votes.end()) - votes.begin());
            jolt.offsets[slot] = known && votes[winner] * 2 > known ? std::int8_t(winner - 1) : UNREAD;
        }
        jolts.push_back(jolt);
        start = stop;
    }
    return jolts;
}

std::vector<Jolt> find_heart_jolts(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& healths,
                                   const std::vector<double>& times) {
    std::vector<Jolt> jolts;
    std::optional<Jolt> tick;
    int last = 0;
    // a tick too faint to be matched alone is left to find_heart_chains
    const auto close = [&] {
        if (tick && std::count(tick->offsets.begin(), tick->offsets.end(), 1) &&
            std::count(tick->offsets.begin(), tick->offsets.end(), UNREAD) <= SHANKS - MIN_READ_SHANKS) {
            tick->frames = last - tick->frame + 1;
            jolts.push_back(*tick);
        }
        tick.reset();
    };

    for (int frame = 0; frame < int(healths.size()); ++frame) {
        const std::int8_t* seen = states.data() + size_t(frame) * SHANKS;
        if (SHANKS - std::count(seen, seen + SHANKS, UNREAD) < MIN_READ_HEARTS) continue;
        bool same = tick.has_value();
        for (int slot = 0; slot < SHANKS && same; ++slot) {
            same = seen[slot] == UNREAD || tick->offsets[slot] == UNREAD || seen[slot] == tick->offsets[slot];
        }
        if (!same) {
            close();
            tick = Jolt{Row::Hearts, frame, times[frame], 1, {}, healths[frame]};
            tick->offsets.fill(UNREAD);
        }
        for (int slot = 0; slot < SHANKS; ++slot) {
            if (seen[slot] != UNREAD) tick->offsets[slot] = seen[slot];
        }
        last = frame;
    }
    close();
    return jolts;
}

std::vector<Jolt> find_heart_chains(const std::vector<std::int8_t>& states, const std::vector<std::int8_t>& healths,
                                    const std::vector<double>& times, const std::vector<Jolt>& heart_jolts,
                                    int first_chain) {
    // a heart jolt's tick, and a tick either side as frames near it can show it too. Its frame count
    // can run on past skipped frames, so it's no guide
    std::vector<char> taken(healths.size());
    for (const Jolt& jolt : heart_jolts) {
        const double tick = 1.0 / TICKS_PER_SECOND;
        const auto from = std::upper_bound(times.begin(), times.end(), jolt.time - tick);
        const auto to = std::lower_bound(times.begin(), times.end(), jolt.time + 2 * tick);
        std::fill(taken.begin() + (from - times.begin()), taken.begin() + (to - times.begin()), char(1));
    }
    std::vector<int> faint;
    for (int frame = 0; frame < int(healths.size()); ++frame) {
        const std::int8_t* seen = states.data() + size_t(frame) * SHANKS;
        const int read = SHANKS - int(std::count(seen, seen + SHANKS, UNREAD));
        if (!taken[frame] && read > 0) faint.push_back(frame);
    }

    // frames a tick lasts at this frame rate
    int tick_frames = 1;
    if (times.size() > 1) {
        std::vector<double> gaps(times.size());
        std::adjacent_difference(times.begin(), times.end(), gaps.begin());
        const auto middle = gaps.begin() + gaps.size() / 2;
        std::nth_element(gaps.begin() + 1, middle, gaps.end());
        if (*middle > 0) tick_frames = std::max(1, int(std::lround(1 / (TICKS_PER_SECOND * *middle))));
    }

    struct Tally {
        int frame = -1, frames = 0;
        std::array<std::array<int, 2>, SHANKS> votes{};
    };
    // frames from up to to, tallied per tick
    const auto ticks_of = [&](size_t from, size_t to) {
        const double start = times[faint[from]];
        // where ticks begin between frames is unknown, the right guess never splits a tick's frames
        const auto tally = [&](int phase) {
            const auto tick_of = [&](int frame) {
                return int(std::floor((times[frame] - start) * TICKS_PER_SECOND + (phase + 0.5) / PHASES));
            };
            std::vector<Tally> ticks(tick_of(faint[to - 1]) + 1);
            for (size_t at = from; at < to; ++at) {
                Tally& tick = ticks[tick_of(faint[at])];
                if (tick.frame < 0) tick.frame = faint[at];
                ++tick.frames;
                const std::int8_t* seen = states.data() + size_t(faint[at]) * SHANKS;
                for (int slot = 0; slot < SHANKS; ++slot) {
                    if (seen[slot] != UNREAD) ++tick.votes[slot][seen[slot]];
                }
            }
            return ticks;
        };
        std::vector<Tally> ticks;
        int fewest = std::numeric_limits<int>::max();
        for (int phase = 0; phase < PHASES; ++phase) {
            std::vector<Tally> tried = tally(phase);
            int split = 0;
            for (const Tally& tick : tried) {
                for (const auto& votes : tick.votes) split += std::min(votes[0], votes[1]);
            }
            if (split < fewest) {
                fewest = split;
                ticks = std::move(tried);
            }
        }
        return ticks;
    };
    // one jolt per tick with a lowered heart, hearts at rest read all level so only those show they jolt
    const auto jolts_of = [&](const std::vector<Tally>& ticks) {
        std::vector<Jolt> found;
        for (int tick = 0; tick < int(ticks.size()); ++tick) {
            const Tally& counted = ticks[tick];
            if (counted.frame < 0) continue;
            Jolt jolt{Row::Hearts, counted.frame, times[counted.frame], counted.frames, {}, healths[counted.frame]};
            for (int slot = 0; slot < SHANKS; ++slot) {
                const auto& votes = counted.votes[slot];
                jolt.offsets[slot] = votes[0] > votes[1] ? 0 : votes[1] > votes[0] ? 1 : UNREAD;
                // in the dark a heart read on one frame, outvoted or on a tick missing frames is wrong up to a third
                // of the time, mostly a frame still showing the tick before, else under one in 300
                const bool steady = std::min(votes[0], votes[1]) == 0 && votes[0] + votes[1] >= 2 &&
                                    counted.frames >= tick_frames;
                if (jolt.offsets[slot] != UNREAD && !steady) jolt.unsure |= std::uint16_t(1 << slot);
            }
            if (std::count(jolt.offsets.begin(), jolt.offsets.end(), 1) == 0) continue;
            jolt.link = tick;
            found.push_back(jolt);
        }
        return found;
    };
    // a heart read steadily on enough ticks places a chain alone, however misread the others
    const auto placeable = [&](const std::vector<Jolt>& found) {
        std::array<int, SHANKS> steady{};
        for (const Jolt& jolt : found) {
            for (int slot = 0; slot < SHANKS; ++slot) {
                steady[slot] += jolt.offsets[slot] != UNREAD && !(jolt.unsure >> slot & 1);
            }
        }
        return *std::max_element(steady.begin(), steady.end()) >= MIN_CHAIN_BITS;
    };
    const auto lowered = [&](int frame) {
        const std::int8_t* seen = states.data() + size_t(frame) * SHANKS;
        return std::find(seen, seen + SHANKS, std::int8_t(1)) != seen + SHANKS;
    };
    // a heart read on both frames and different, so they show different ticks whatever the phase
    const auto changes = [&](int before, int after) {
        const std::int8_t* was = states.data() + size_t(before) * SHANKS;
        const std::int8_t* is = states.data() + size_t(after) * SHANKS;
        for (int slot = 0; slot < SHANKS; ++slot) {
            if (was[slot] != UNREAD && is[slot] != UNREAD && was[slot] != is[slot]) return true;
        }
        return false;
    };

    std::vector<Jolt> jolts;
    int chain = first_chain;
    for (size_t from = 0, to, end = 0; from < faint.size(); from = to) {
        if (from >= end) {
            end = from + 1;
            while (end < faint.size() && times[faint[end]] - times[faint[end - 1]] <= MAX_CHAIN_GAP) ++end;
        }
        // frames before the first lowered heart would only use up the window
        if (!lowered(faint[from])) {
            to = from + 1;
            continue;
        }
        const double start = times[faint[from]];
        // cut where the hearts are seen to change, so the next window doesn't start on the rest of a tick
        for (to = from + 1; to < end && times[faint[to]] - start < CHAIN_SECONDS; ++to) {}
        while (to < end && times[faint[to]] - start < CHAIN_SECONDS + MAX_CHAIN_GAP &&
               !changes(faint[to - 1], faint[to])) {
            ++to;
        }
        // the rest of the run joins this window when it couldn't be placed alone
        const bool short_rest = to < end && times[faint[end - 1]] - times[faint[to]] < CHAIN_SECONDS;
        if (short_rest && !placeable(jolts_of(ticks_of(to, end)))) to = end;

        std::vector<Jolt> found = jolts_of(ticks_of(from, to));
        int bits = 0;
        for (const Jolt& jolt : found) {
            bits += SHANKS - int(std::count(jolt.offsets.begin(), jolt.offsets.end(), UNREAD));
        }
        if (bits < MIN_CHAIN_BITS) continue;
        const int first = found.front().link;
        for (Jolt& jolt : found) {
            jolt.chain = chain;
            jolt.link -= first;
        }
        jolts.insert(jolts.end(), found.begin(), found.end());
        ++chain;
    }
    return jolts;
}

namespace {

struct Hit {
    double score = -1, x = 0, y = 0, scale = 0;
};

Image grey_of(const Frame& frame) {
    Image grey(frame.width, frame.height, 1);
    for (size_t pixel = 0; pixel < grey.data.size(); ++pixel) {
        const std::uint8_t* bgr = frame.pixels.data() + pixel * 3;
        grey.data[pixel] = 0.114f * bgr[0] + 0.587f * bgr[1] + 0.299f * bgr[2];
    }
    return grey;
}

Image shrunk(const Image& image, int left, int top, int width, int height, int factor) {
    Image out(width / factor, height / factor, 1);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            float sum = 0;
            for (int dy = 0; dy < factor; ++dy) {
                const float* row = image.at(left + x * factor, top + y * factor + dy);
                for (int dx = 0; dx < factor; ++dx) sum += row[dx];
            }
            *out.at(x, y) = sum / float(factor * factor);
        }
    }
    return out;
}

struct ShankKernel {
    struct Cell {
        int x, y;
        float weight;
        std::array<float, LOOKS> looks;
    };

    int size;
    std::vector<Cell> cells;
    double total = 0;
    std::array<double, LOOKS> energy{};

    explicit ShankKernel(int size) : size(size) {
        const double scale = double(size) / sprites::SIZE;
        Image weight(size, size, 1);
        paint(icon(Row::Food, 0, 0), nullptr, 0, 0, scale, weight, nullptr);
        for (float value : weight.data) total += value;

        std::array<Image, LOOKS> shapes;
        for (int look = 0; look < LOOKS; ++look) {
            const Icon& drawn = icon(Row::Food, look / FILLS, look % FILLS);
            Image coverage(size, size, 1);
            shapes[look] = Image(size, size, 1);
            paint(drawn, drawn.grey.data(), 0, 0, scale, coverage, &shapes[look]);
            divide_by_coverage(shapes[look], weight);
            double mean = 0;
            for (size_t pixel = 0; pixel < weight.data.size(); ++pixel) {
                mean += weight.data[pixel] * shapes[look].data[pixel] / total;
            }
            for (size_t pixel = 0; pixel < weight.data.size(); ++pixel) {
                shapes[look].data[pixel] -= float(mean);
                energy[look] += weight.data[pixel] * shapes[look].data[pixel] * shapes[look].data[pixel];
            }
        }
        for (int pixel = 0; pixel < size * size; ++pixel) {
            if (weight.data[pixel] <= 0) continue;
            Cell cell{pixel % size, pixel / size, weight.data[pixel], {}};
            for (int look = 0; look < LOOKS; ++look) cell.looks[look] = cell.weight * shapes[look].data[pixel];
            cells.push_back(cell);
        }
    }
};

Image response(const Image& grey, const ShankKernel& kernel) {
    Image out(std::max(0, grey.width - kernel.size + 1), std::max(0, grey.height - kernel.size + 1), 1);
    const size_t wide = size_t(out.width);
    // a whole row at once so the inner loop vectorises, same add order so scores match to the bit
    std::vector<float> sums(wide), squares(wide), matches(wide * LOOKS);
    for (int y = 0; y < out.height; ++y) {
        std::fill(sums.begin(), sums.end(), 0.0f);
        std::fill(squares.begin(), squares.end(), 0.0f);
        std::fill(matches.begin(), matches.end(), 0.0f);
        for (const ShankKernel::Cell& cell : kernel.cells) {
            const float* values = grey.at(cell.x, y + cell.y);
            for (size_t x = 0; x < wide; ++x) {
                sums[x] += cell.weight * values[x];
                squares[x] += cell.weight * values[x] * values[x];
            }
            for (int look = 0; look < LOOKS; ++look) {
                // local copy or the compiler won't vectorise
                const float weight = cell.looks[look];
                float* match = matches.data() + look * wide;
                for (size_t x = 0; x < wide; ++x) match[x] += weight * values[x];
            }
        }
        for (size_t x = 0; x < wide; ++x) {
            const double spread =
                std::max(squares[x] - double(sums[x]) * sums[x] / kernel.total, kernel.total * MIN_STD * MIN_STD);
            double best = -1;
            for (int look = 0; look < LOOKS; ++look) {
                best = std::max(best, matches[look * wide + x] / std::sqrt(kernel.energy[look] * spread));
            }
            *out.at(int(x), y) = float(best);
        }
    }
    return out;
}

Image widened_peaks(const Image& image) {
    Image out(image.width, image.height, 1);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            float peak = -1;
            for (int ny = std::max(0, y - 1); ny <= std::min(image.height - 1, y + 1); ++ny) {
                for (int nx = std::max(0, x - 1); nx <= std::min(image.width - 1, x + 1); ++nx) {
                    peak = std::max(peak, *image.at(nx, ny));
                }
            }
            *out.at(x, y) = peak;
        }
    }
    return out;
}

Hit strongest_row(const Image& scores, int size, double stretch_range) {
    const Image peaks = widened_peaks(scores);
    Hit best;
    for (double stretch = 1 - stretch_range; stretch <= 1 + stretch_range + 1e-9; stretch += 0.005) {
        const double scale = double(size) / sprites::SIZE * stretch;
        std::array<int, SHANKS> starts;
        for (int slot = 0; slot < SHANKS; ++slot) starts[slot] = int(std::nearbyint(slot * SPACING * scale));
        const int usable = peaks.width - starts[SHANKS - 1];
        for (int y = 0; y < peaks.height; ++y) {
            for (int x = 0; x < usable; ++x) {
                float sum = 0;
                for (int start : starts) sum += *peaks.at(x + start, y);
                if (sum / SHANKS > best.score) best = {sum / SHANKS, double(x), double(y), scale};
            }
        }
    }
    return best;
}

int shrink_factor(int size, int detail) { return std::max(1, size / detail); }

Hit search(const Image& grey, int left, int top, int right, int bottom, int size, int detail, double stretch_range) {
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, grey.width);
    bottom = std::min(bottom, grey.height);
    if (right - left < size || bottom - top < size) return {};

    const int factor = shrink_factor(size, detail);
    const Image small = shrunk(grey, left, top, right - left, bottom - top, factor);
    const Hit hit = strongest_row(response(small, ShankKernel(size / factor)), size / factor, stretch_range);
    if (hit.score < 0) return hit;
    return {hit.score, left + hit.x * factor, top + hit.y * factor, hit.scale * factor};
}

Hit search_near(const Image& grey, double x, double y, double scale, int size, double slack) {
    const int pad = int(scale * 1.5) + 6;
    return search(grey, int(x) - pad, int(y) - pad, int(x + BAR_WIDTH * scale * (1 + slack)) + pad,
                  int(y + sprites::SIZE * scale) + pad, size, FINE, 0.04);
}

template <typename Visit>
void for_each_size(int detail, Visit visit) {
    for (int size = MIN_SIZE; size <= MAX_SIZE; ++size) {
        if (size % shrink_factor(size, detail) == 0) visit(size);
    }
}

Hit search_vanilla_spot(const Image& grey) {
    Hit best;
    for_each_size(FINE, [&](int size) {
        const double scale = double(size) / sprites::SIZE;
        const Hit hit = search_near(grey, grey.width / 2.0 + 10 * scale, grey.height - 39 * scale, scale, size, 0.04);
        if (hit.score > best.score) best = hit;
    });
    return best;
}

Hit search_anywhere(const Image& grey) {
    std::vector<Hit> rough;
    for_each_size(COARSE, [&](int size) {
        rough.push_back(search(grey, 0, grey.height / 2, grey.width, grey.height, size, COARSE, 0.06));
    });
    std::sort(rough.begin(), rough.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    rough.resize(std::min<size_t>(rough.size(), PROPOSALS));

    Hit best;
    for (const Hit& proposal : rough) {
        if (proposal.score < 0) continue;
        for_each_size(FINE, [&](int size) {
            if (std::abs(size / (sprites::SIZE * proposal.scale) - 1) > 0.08) return;
            const Hit hit = search_near(grey, proposal.x, proposal.y, proposal.scale, size, 0.08);
            if (hit.score > best.score) best = hit;
        });
    }
    return best;
}

template <typename Place, typename Lead>
bool same_bar(const Place& place, const Lead& lead) {
    return std::abs(place.x - lead.x) <= lead.scale && std::abs(place.y - lead.y) <= lead.scale &&
           std::abs(place.scale / lead.scale - 1) <= 0.03;
}

std::optional<std::pair<Bar, std::vector<int>>> agree(const std::vector<Hit>& hits) {
    std::vector<int> votes;
    for (const Hit& lead : hits) {
        if (lead.score < MIN_ROW_SCORE) continue;
        std::vector<int> same;
        for (int index = 0; index < int(hits.size()); ++index) {
            const Hit& hit = hits[index];
            if (hit.score >= MIN_ROW_SCORE && same_bar(hit, lead)) same.push_back(index);
        }
        if (same.size() > votes.size()) votes = same;
    }
    if (int(votes.size()) < MIN_VOTES) return std::nullopt;

    std::vector<float> xs, ys, scales;
    for (int index : votes) {
        xs.push_back(float(hits[index].x));
        ys.push_back(float(hits[index].y));
        scales.push_back(float(hits[index].scale));
    }
    return std::pair{Bar{median(xs), median(ys), median(scales)}, votes};
}

Bar refine(const Bar& bar, const std::vector<const Frame*>& frames) {
    const Rect rect = bar.rect();
    const int pad = int(4 * bar.scale);
    const int left = std::max(rect.left - pad, 0), top = std::max(rect.top - pad, 0);
    const int width = std::min(rect.width + 2 * pad, frames[0]->width - left);
    const int height = std::min(rect.height + 2 * pad, frames[0]->height - top);

    Image still(width, height, 3);
    std::vector<float> samples;
    for (int y = 0; y < height; ++y) {
        for (int value = 0; value < width * 3; ++value) {
            samples.clear();
            for (const Frame* frame : frames) {
                samples.push_back(frame->pixels[(size_t(top + y) * frame->width + left) * 3 + value]);
            }
            still.at(0, y)[value] = median(samples);
        }
    }

    const auto fit = [&](const std::array<double, 3>& place) {
        const BarModel model(Bar{place[0] - left, place[1] - top, place[2]}, Row::Food);
        const Rect& strip = model.rect();
        if (strip.left < 0 || strip.top < 0 || strip.left + strip.width > width || strip.top + strip.height > height) {
            return -1.0;
        }
        Image crop(strip.width, strip.height, 3);
        for (int y = 0; y < strip.height; ++y) {
            std::copy_n(still.at(strip.left, strip.top + y), strip.width * 3, crop.at(0, y));
        }
        return model.read(crop).presence;
    };

    std::array<double, 3> best = {bar.x, bar.y, bar.scale};
    double score = fit(best);
    std::array<double, 3> steps = {1.0, 1.0, bar.scale * 0.01};
    while (steps[0] >= 0.125) {
        bool improved = false;
        for (int axis = 0; axis < 3; ++axis) {
            for (int direction : {1, -1}) {
                std::array<double, 3> trial = best;
                trial[axis] += direction * steps[axis];
                const double found = fit(trial);
                if (found > score) {
                    best = trial;
                    score = found;
                    improved = true;
                }
            }
        }
        if (!improved) {
            for (double& step : steps) step /= 2;
        }
    }
    return {best[0], best[1], best[2]};
}

std::vector<Image> greys_of(const std::vector<Frame>& frames) {
    std::vector<Image> greys(frames.size());
    for_each_index(int(frames.size()), SEARCH_THREADS, [&](int index) { greys[index] = grey_of(frames[index]); });
    return greys;
}

std::optional<Bar> agreed(const std::vector<Frame>& frames, const std::vector<Image>& greys, bool anywhere) {
    std::vector<Hit> hits(frames.size());
    for_each_index(int(frames.size()), SEARCH_THREADS, [&](int index) {
        hits[index] = anywhere ? search_anywhere(greys[index]) : search_vanilla_spot(greys[index]);
    });
    const auto found = agree(hits);
    if (!found) return std::nullopt;
    std::vector<const Frame*> supporters;
    for (int index : found->second) supporters.push_back(&frames[index]);
    return refine(found->first, supporters);
}

Rect vanilla_band(const VideoInfo& info) {
    const double largest = double(MAX_SIZE) / sprites::SIZE;
    const int tall = std::min(info.height, int(std::ceil(39 * largest + 1.5 * largest + 6)));
    return {info.width / 2, info.height - tall, info.width - info.width / 2, tall};
}

bool holds(const Rect& outer, const Rect& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.left + inner.width <= outer.left + outer.width &&
           inner.top + inner.height <= outer.top + outer.height;
}

}  // namespace

std::optional<Bar> locate(const std::vector<Frame>& frames) {
    const std::vector<Image> greys = greys_of(frames);
    for (bool anywhere : {false, true}) {
        if (const std::optional<Bar> bar = agreed(frames, greys, anywhere)) return bar;
    }
    return std::nullopt;
}

Bar find_bar(const std::string& path, const VideoInfo& info, int first, int last) {
    std::vector<Frame> frames(SAMPLE_FRAMES);
    std::vector<std::string> why(SAMPLE_FRAMES);
    for_each_index(SAMPLE_FRAMES, SAMPLE_FRAMES, [&](int index) {
        const double position = 0.05 + 0.9 * index / (SAMPLE_FRAMES - 1);
        frames[index] = frame_at(path, info, info.times[first + int((last - first) * position)], &why[index]);
    });
    std::erase_if(frames, [](const Frame& frame) { return frame.pixels.empty(); });
    if (frames.empty()) {
        throw Problem(code::NO_FRAMES, "ffmpeg gave no pictures from the video",
                      "ffmpeg starts but nothing comes out of it. Mostly that is a broken ffmpeg: delete ffmpeg.exe "
                      "and ffprobe.exe next to Jolt so it downloads fresh ones, or update the ffmpeg installed on "
                      "this PC. If a video player cannot play the file either, the file is what is broken.",
                      why.front());
    }
    const std::optional<Bar> bar = locate(frames);
    if (!bar) {
        throw Problem(code::NO_BAR, "Jolt found no hunger bar in the video",
                      "It looks at " + std::to_string(SAMPLE_FRAMES) +
                          " pictures spread over the part that is read. The bar is not in them when the "
                      "HUD is switched off, in creative or spectator mode, where the video is not gameplay, or when "
                      "a mod or resource pack moves or redraws the bar. Set where to start and stop reading to a "
                      "stretch of survival gameplay, or give the bar's place yourself as x,y,scale (--bar).");
    }
    return *bar;
}

std::vector<Bar> find_bars(const std::string& path, const VideoInfo& info, int first, int last, double fps, int jobs) {
    std::vector<Bar> bars = {find_bar(path, info, first, last)};
    const Rect band = vanilla_band(info);
    const BarModel known(bars[0], Row::Food);
    if (!info.keyframes_alone || !holds(band, known.rect())) return bars;

    std::vector<int> keyframes;
    for (int keyframe : info.keyframes) {
        if (keyframe >= first && keyframe < last) keyframes.push_back(keyframe);
    }
    struct Sighting {
        double time;
        Hit hit;
    };
    std::vector<Sighting> sightings;
    std::mutex guard;
    // keyframe times are off by up to this, see pieces_worth_reading
    const double reach = info.reorder + 0.5 / fps;
    for_each_index(jobs, jobs, [&](int job) {
        const int count = int(keyframes.size());
        const int from = int(std::int64_t(count) * job / jobs), to = int(std::int64_t(count) * (job + 1) / jobs);
        if (from >= to) return;
        const double end = to < count ? info.times[keyframes[to]] : info.times[last - 1] + 1 / fps;
        StripReader reader(path, band, std::max(0.0, info.times[keyframes[from]] - reach), to - from, {}, end - 0.5 / fps);
        std::vector<std::uint8_t> frame(size_t(band.width) * band.height * 3);
        const Rect& strip = known.rect();
        Image seen(strip.width, strip.height, 3), grey(info.width, info.height, 1);
        while (reader.read(frame.data(), 1)) {
            const auto pixel = [&](int x, int y) { return frame.data() + (size_t(y - band.top) * band.width + x - band.left) * 3; };
            for (int y = 0; y < strip.height; ++y) {
                std::copy_n(pixel(strip.left, strip.top + y), strip.width * 3, seen.at(0, y));
            }
            if (known.read(seen).presence >= MIN_FRAME_PRESENCE) continue;

            for (int y = band.top; y < band.top + band.height; ++y) {
                for (int x = band.left; x < band.left + band.width; ++x) {
                    const std::uint8_t* bgr = pixel(x, y);
                    *grey.at(x, y) = 0.114f * bgr[0] + 0.587f * bgr[1] + 0.299f * bgr[2];
                }
            }
            const Hit hit = search_vanilla_spot(grey);
            if (hit.score < MIN_ROW_SCORE || same_bar(hit, bars[0])) continue;
            const std::lock_guard<std::mutex> lock(guard);
            sightings.push_back({reader.time(), hit});
        }
    });
    // decoders finish in any order, sort so a video always gives the same bars
    std::sort(sightings.begin(), sightings.end(), [](const Sighting& a, const Sighting& b) { return a.time < b.time; });

    for (int tries = 0; tries < OTHER_TRIES && !sightings.empty(); ++tries) {
        std::vector<int> votes;
        for (const Sighting& lead : sightings) {
            std::vector<int> with;
            for (int index = 0; index < int(sightings.size()); ++index) {
                if (same_bar(sightings[index].hit, lead.hit)) with.push_back(index);
            }
            if (with.size() > votes.size()) votes = with;
        }
        std::vector<double> times;
        const int step = std::max(1, int(votes.size()) / FOLLOWED_KEYFRAMES);
        for (int at = 0; at < int(votes.size()) && at < step * FOLLOWED_KEYFRAMES; at += step) {
            for (double later : LATER) times.push_back(sightings[votes[at]].time + later);
        }
        std::vector<Frame> frames(times.size());
        for_each_index(int(times.size()), int(times.size()), [&](int index) { frames[index] = frame_at(path, info, times[index]); });
        std::erase_if(frames, [](const Frame& frame) { return frame.pixels.empty(); });
        // one keyframe isn't enough, frames have to agree like they do for the first bar
        const std::optional<Bar> bar = agreed(frames, greys_of(frames), false);
        if (bar && std::none_of(bars.begin(), bars.end(), [&](const Bar& have) { return same_bar(*bar, have); })) {
            bars.push_back(*bar);
        }
        for (auto vote = votes.rbegin(); vote != votes.rend(); ++vote) sightings.erase(sightings.begin() + *vote);
    }
    return bars;
}

}  // namespace jolt
