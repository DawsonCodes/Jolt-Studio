#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace jolt {

struct Problem : std::runtime_error {
    Problem(const char* code, const std::string& what, std::string advice, std::string details = {})
        : std::runtime_error(what), code(code), advice(std::move(advice)), details(std::move(details)) {}

    // J01 etc, the guide has a section per code
    const char* code;
    std::string advice;
    // commands, exit codes, tool output
    std::string details;
};

namespace code {
inline constexpr const char* TOOLS_DECLINED = "J01";
inline constexpr const char* DOWNLOAD_FAILED = "J02";
inline constexpr const char* CANNOT_START = "J03";
inline constexpr const char* NO_SUCH_VIDEO = "J04";
inline constexpr const char* UNREADABLE_VIDEO = "J05";
inline constexpr const char* LINK_FAILED = "J06";
inline constexpr const char* NO_BAR = "J07";
inline constexpr const char* NO_FRAMES = "J08";
inline constexpr const char* CANNOT_WRITE = "J09";
inline constexpr const char* PART_UNREAD = "J10";
inline constexpr const char* BAD_SETTING = "J11";
inline constexpr const char* UPDATE_FAILED = "J12";
inline constexpr const char* CRASHED = "J20";
inline constexpr const char* UNEXPECTED = "J99";
}  // namespace code

void set_window(std::uintptr_t handle);
std::uintptr_t window();

bool own_console();

// boxes only with a jolt window or our own console, scripts and terminals just get stderr
bool boxes();

void tell(const Problem& problem);
void warn(const Problem& problem);
void inform(const std::string& text);
bool confirm(const std::string& text);

std::string windows_error(unsigned long number);
// empty for ordinary exit codes
std::string exit_meaning(unsigned code);
std::string tail(const std::string& text);

// crashes write jolt-crash.dmp next to the exe
void report_crashes();

}  // namespace jolt
