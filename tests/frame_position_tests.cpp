#include "report.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

int main() {
    int checks = 0;
    const auto check = [&](const std::vector<double>& times, int number, int second, int within) {
        const auto got = jolt::frame_position(times, number);
        if (got.number != number || got.second != second || got.within_second != within ||
            got.time != times[number]) throw std::runtime_error("wrong source frame position");
        ++checks;
    };
    std::vector<double> sixty, thirty, fractional;
    for (int n = 0; n < 3602; ++n) {
        sixty.push_back(n / 60.0);
        thirty.push_back(n / 30.0);
        fractional.push_back(n * 1001.0 / 30000);
    }
    check(sixty, 0, 0, 0);
    check(sixty, 59, 0, 59);
    check(sixty, 60, 1, 0);
    check(sixty, 3601, 60, 1);
    check(thirty, 29, 0, 29);
    check(thirty, 30, 1, 0);
    check(fractional, 29, 0, 29);
    check(fractional, 30, 1, 0);
    check(fractional, 3000, 100, 2);
    const std::vector<double> variable = {0, .2, .201, .8, .999, 1, 1.01, 1.8, 3.1};
    check(variable, 4, 0, 4);
    check(variable, 5, 1, 0);
    check(variable, 7, 1, 2);
    check(variable, 8, 3, 0);
    check({0, .5, 1 - 1e-12, 1.25}, 2, 1, 0);
    check({0, 904, 904.01, 904.2, 904.3}, 4, 904, 3);
    bool rejected = false;
    try { jolt::frame_position(variable, 9); } catch (const std::out_of_range&) { rejected = true; }
    if (!rejected) return 1;
    std::printf("%d source frame checks pass.\n", checks);
}
