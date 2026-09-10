#pragma once

// Small shared helpers for the command line front ends.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "lab_scheduler/canonical.hpp"
#include "lab_scheduler/error.hpp"

namespace lab_scheduler {

inline bool has_flag(const std::vector<std::string>& arguments, const std::string& flag) {
    for (const std::string& argument : arguments) {
        if (argument == flag) {
            return true;
        }
    }
    return false;
}

inline std::string option_value(const std::vector<std::string>& arguments, const std::string& name,
                                const std::string& fallback = std::string()) {
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == name) {
            return arguments[i + 1];
        }
    }
    return fallback;
}

inline Result<std::uint32_t> option_u32(const std::vector<std::string>& arguments, const std::string& name,
                                        std::uint32_t fallback) {
    const std::string raw = option_value(arguments, name);
    if (raw.empty()) {
        return fallback;
    }
    std::uint32_t value = 0;
    if (!parse_decimal_u32(raw, value)) {
        return Status(ErrorCode::InvalidArgument, "invalid value for " + name + ": " + raw);
    }
    return value;
}

inline std::vector<std::string> positional_arguments(const std::vector<std::string>& arguments,
                                                     const std::vector<std::string>& valued_options) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        bool consumes = false;
        for (const std::string& option : valued_options) {
            if (arguments[i] == option) {
                consumes = true;
                break;
            }
        }
        if (consumes) {
            ++i;
            continue;
        }
        if (!arguments[i].empty() && arguments[i][0] == '-') {
            continue;
        }
        out.push_back(arguments[i]);
    }
    return out;
}

// Disables the CRT error dialog so a failure never opens a visible window.
inline void suppress_error_dialogs() noexcept {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace lab_scheduler
