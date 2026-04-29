#include "build.h"

#include <array>
#include <cstdio>
#include <sys/wait.h>

namespace patchwork {

BuildResult RunBuildCommand(std::string_view command) {
    BuildResult result;
    if (command.empty()) {
        result.output = "No build command configured.";
        return result;
    }

    std::string shell_command(command);
    shell_command.append(" 2>&1");

    FILE* pipe = popen(shell_command.c_str(), "r");
    if (pipe == nullptr) {
        result.output = "Failed to start build command.";
        return result;
    }

    result.ran = true;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result.output.append(buffer.data());
    }

    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = status;
    }
    return result;
}

}  // namespace patchwork

