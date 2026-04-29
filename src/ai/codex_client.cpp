#include "ai/codex_client.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include "ai/prompt.h"
#include "diff.h"

namespace patchwork {

namespace {

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

std::string ShellEscapeSingleQuoted(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() + 8);
    escaped.push_back('\'');
    for (char ch : input) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

CommandResult RunCommand(const std::string& command) {
    CommandResult result;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        result.exit_code = 1;
        result.output = "Failed to launch codex CLI.";
        return result;
    }

    std::array<char, 512> buffer{};
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

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::filesystem::path MakeTempFile(std::string_view suffix, const std::string& contents, std::string* error) {
    std::string pattern =
        (std::filesystem::temp_directory_path() / ("patchwork-codex-XXXXXX" + std::string(suffix))).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');

    const int fd = mkstemps(buffer.data(), static_cast<int>(suffix.size()));
    if (fd == -1) {
        if (error != nullptr) {
            *error = "Failed to create a temporary file for codex.";
        }
        return {};
    }

    close(fd);
    const std::filesystem::path path(buffer.data());
    std::ofstream output(path);
    if (!output) {
        if (error != nullptr) {
            *error = "Failed to write a temporary file for codex.";
        }
        std::filesystem::remove(path);
        return {};
    }
    output << contents;
    return path;
}

AiResponseKind ClassifyResponse(const std::string& raw_text, const std::string& diff_text) {
    if (diff_text.empty()) {
        return AiResponseKind::ExplanationOnly;
    }
    if (raw_text == diff_text) {
        return AiResponseKind::DiffOnly;
    }
    return AiResponseKind::DiffWithExplanation;
}

std::filesystem::path ResolveWorkingDirectory(const AiRequest& request) {
    const std::filesystem::path file_path(request.file_path);
    if (!request.file_path.empty()) {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(file_path, error);
        if (!error && !absolute.empty()) {
            const std::filesystem::path parent = absolute.parent_path();
            if (!parent.empty()) {
                return parent;
            }
        }
    }
    return std::filesystem::current_path();
}

}  // namespace

CodexClient::CodexClient() {
    const char* model = std::getenv("PATCHWORK_CODEX_MODEL");
    const char* profile = std::getenv("PATCHWORK_CODEX_PROFILE");
    model_ = (model != nullptr && *model != '\0') ? model : "";
    profile_ = (profile != nullptr && *profile != '\0') ? profile : "";
}

AiResponse CodexClient::Complete(const AiRequest& request) {
    std::string error;
    const std::string prompt = BuildPrompt(request);
    const std::filesystem::path prompt_path = MakeTempFile(".prompt", prompt, &error);
    if (!error.empty()) {
        return {.kind = AiResponseKind::Error, .error_message = error};
    }

    const std::filesystem::path output_path = MakeTempFile(".out", "", &error);
    if (!error.empty()) {
        std::filesystem::remove(prompt_path);
        return {.kind = AiResponseKind::Error, .error_message = error};
    }

    const std::filesystem::path workdir = ResolveWorkingDirectory(request);
    std::string command = "codex exec --sandbox read-only --skip-git-repo-check --ephemeral --color never";
    if (!profile_.empty()) {
        command += " --profile " + ShellEscapeSingleQuoted(profile_);
    }
    if (!model_.empty()) {
        command += " --model " + ShellEscapeSingleQuoted(model_);
    }
    command += " --cd " + ShellEscapeSingleQuoted(workdir.string());
    command += " --output-last-message " + ShellEscapeSingleQuoted(output_path.string());
    command += " - < " + ShellEscapeSingleQuoted(prompt_path.string()) + " 2>&1";

    const CommandResult result = RunCommand(command);
    const std::string raw_text = ReadFile(output_path);

    std::error_code remove_error;
    std::filesystem::remove(prompt_path, remove_error);
    std::filesystem::remove(output_path, remove_error);

    if (result.exit_code != 0) {
        const std::string message = !result.output.empty() ? result.output
                                                           : (!raw_text.empty() ? raw_text
                                                                                : "Codex CLI request failed.");
        return {.kind = AiResponseKind::Error, .error_message = message};
    }

    const std::string final_text = !raw_text.empty() ? raw_text : result.output;
    if (final_text.empty()) {
        return {.kind = AiResponseKind::Error, .error_message = "Codex CLI returned no text."};
    }

    const std::string diff_text = ExtractDiffText(final_text);
    AiResponse response;
    response.kind = ClassifyResponse(final_text, diff_text);
    response.raw_text = final_text;
    if (!diff_text.empty()) {
        response.diff_text = diff_text;
    }
    return response;
}

}  // namespace patchwork
