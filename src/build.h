#pragma once

#include <string>
#include <string_view>

namespace flowstate {

struct BuildResult {
    int exit_code = 0;
    std::string output;
    bool ran = false;
};

BuildResult RunBuildCommand(std::string_view command);

}  // namespace flowstate

