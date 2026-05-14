#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace flowstate {

struct RateLimitWindowInfo {
    bool available = false;
    double used_percent = 0.0;
    std::optional<int64_t> window_duration_mins;
    std::optional<int64_t> resets_at;
};

struct RateLimitSnapshotInfo {
    bool available = false;
    std::string limit_id;
    std::string limit_name;
    RateLimitWindowInfo primary;
    RateLimitWindowInfo secondary;
};

}  // namespace flowstate
