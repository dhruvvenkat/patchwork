#include "git_status.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <sys/wait.h>

namespace patchwork {

namespace {

std::string TrimTrailingNewlines(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

std::string ShellQuote(const std::string& text) {
    std::string quoted = "'";
    for (const char ch : text) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string RunCommand(const std::string& command, int* exit_code = nullptr) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        if (exit_code != nullptr) {
            *exit_code = -1;
        }
        return {};
    }

    std::string output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output.append(buffer.data());
    }

    const int status = pclose(pipe);
    if (exit_code != nullptr) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else {
            *exit_code = status;
        }
    }
    return output;
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

bool ParsePositiveInteger(std::string_view text, size_t* value) {
    if (text.empty()) {
        return false;
    }

    size_t parsed = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc() || result.ptr != last) {
        return false;
    }

    if (value != nullptr) {
        *value = parsed;
    }
    return true;
}

bool ParseHunkHeader(std::string_view line, size_t* new_start) {
    const size_t plus = line.find('+');
    if (plus == std::string_view::npos) {
        return false;
    }

    size_t number_end = plus + 1;
    while (number_end < line.size() && line[number_end] >= '0' && line[number_end] <= '9') {
        ++number_end;
    }
    if (number_end == plus + 1) {
        return false;
    }

    return ParsePositiveInteger(line.substr(plus + 1, number_end - plus - 1), new_start);
}

void ApplyMarker(std::vector<GitLineChange>& lines, size_t line, GitLineMarker marker) {
    if (line == 0 || line > lines.size()) {
        return;
    }

    GitLineMarker& current = lines[line - 1].marker;
    const auto rank = [](GitLineMarker value) {
        switch (value) {
            case GitLineMarker::Clean:
                return 0;
            case GitLineMarker::Added:
                return 1;
            case GitLineMarker::Modified:
                return 2;
            case GitLineMarker::Deleted:
                return 3;
        }
        return 0;
    };

    if (rank(marker) > rank(current)) {
        current = marker;
    }
}

void ApplyPreviousLines(std::vector<GitLineChange>& lines,
                        size_t line,
                        GitLineMarker marker,
                        std::vector<std::string> previous_lines) {
    if (line == 0 || line > lines.size() || previous_lines.empty()) {
        return;
    }

    ApplyMarker(lines, line, marker);
    std::vector<std::string>& target = lines[line - 1].previous_lines;
    target.insert(target.end(),
                  std::make_move_iterator(previous_lines.begin()),
                  std::make_move_iterator(previous_lines.end()));
}

void ApplyDeletedLines(std::vector<GitLineChange>& lines,
                       size_t deletion_anchor,
                       std::vector<std::string> deleted_lines) {
    if (lines.empty() || deleted_lines.empty()) {
        return;
    }

    const size_t line = deletion_anchor == 0 ? 1 : std::min(deletion_anchor, lines.size());
    ApplyPreviousLines(lines, line, GitLineMarker::Deleted, std::move(deleted_lines));
}

struct PendingDiffRun {
    size_t deletion_anchor = 0;
    std::vector<std::string> deleted_lines;
    std::vector<size_t> added_lines;
};

void FlushPendingRun(std::vector<GitLineChange>& lines, PendingDiffRun* pending) {
    if (pending == nullptr) {
        return;
    }

    const size_t modified_count = std::min(pending->deleted_lines.size(), pending->added_lines.size());
    for (size_t index = 0; index < pending->added_lines.size(); ++index) {
        if (index < modified_count) {
            ApplyPreviousLines(lines,
                               pending->added_lines[index],
                               GitLineMarker::Modified,
                               {pending->deleted_lines[index]});
        } else {
            ApplyMarker(lines, pending->added_lines[index], GitLineMarker::Added);
        }
    }

    if (pending->deleted_lines.size() > pending->added_lines.size()) {
        std::vector<std::string> unmatched_deleted_lines(
            std::make_move_iterator(pending->deleted_lines.begin() +
                                    static_cast<std::ptrdiff_t>(pending->added_lines.size())),
            std::make_move_iterator(pending->deleted_lines.end()));
        ApplyDeletedLines(lines, pending->deletion_anchor, std::move(unmatched_deleted_lines));
    }

    *pending = {};
}

bool HasAnyOutput(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch != '\n' && ch != '\r' && ch != '\t' && ch != ' ';
    });
}

std::filesystem::path FindGitRoot(const std::filesystem::path& absolute_file_path) {
    const std::filesystem::path search_dir =
        absolute_file_path.has_parent_path() ? absolute_file_path.parent_path() : std::filesystem::current_path();
    int exit_code = 0;
    std::string root = RunCommand("git -C " + ShellQuote(search_dir.string()) +
                                      " rev-parse --show-toplevel 2>/dev/null",
                                  &exit_code);
    if (exit_code != 0) {
        return {};
    }
    root = TrimTrailingNewlines(std::move(root));
    if (root.empty()) {
        return {};
    }
    return std::filesystem::path(root);
}

std::filesystem::path RelativePathForGit(const std::filesystem::path& absolute_file_path,
                                         const std::filesystem::path& git_root) {
    std::error_code error;
    std::filesystem::path relative = std::filesystem::relative(absolute_file_path, git_root, error);
    if (error) {
        return absolute_file_path;
    }
    return relative;
}

}  // namespace

GitLineStatus ParseGitDiffMarkers(std::string_view diff_text, size_t line_count) {
    GitLineStatus status{
        .available = true,
        .lines = std::vector<GitLineChange>(line_count),
    };

    bool in_hunk = false;
    size_t new_line = 0;
    PendingDiffRun pending;
    size_t position = 0;
    while (position <= diff_text.size()) {
        const size_t line_end = diff_text.find('\n', position);
        std::string_view line =
            line_end == std::string_view::npos ? diff_text.substr(position) : diff_text.substr(position, line_end - position);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        size_t parsed_new_start = 0;
        if (line.rfind("@@", 0) == 0 && ParseHunkHeader(line, &parsed_new_start)) {
            FlushPendingRun(status.lines, &pending);
            in_hunk = true;
            new_line = parsed_new_start;
        } else if (in_hunk && !line.empty()) {
            switch (line.front()) {
                case '+':
                    if (line.rfind("+++", 0) != 0) {
                        pending.added_lines.push_back(new_line);
                        ++new_line;
                    }
                    break;
                case '-':
                    if (line.rfind("---", 0) != 0) {
                        if (pending.deleted_lines.empty()) {
                            pending.deletion_anchor = new_line;
                        }
                        pending.deleted_lines.emplace_back(line.substr(1));
                    }
                    break;
                case ' ':
                    FlushPendingRun(status.lines, &pending);
                    ++new_line;
                    break;
                case '\\':
                    break;
                default:
                    FlushPendingRun(status.lines, &pending);
                    in_hunk = false;
                    break;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        position = line_end + 1;
    }

    FlushPendingRun(status.lines, &pending);
    return status;
}

GitLineStatus LoadGitLineStatus(const std::filesystem::path& file_path, size_t line_count) {
    GitLineStatus unavailable{
        .available = false,
        .lines = std::vector<GitLineChange>(line_count),
    };

    if (file_path.empty()) {
        return unavailable;
    }

    const std::filesystem::path absolute_file_path = AbsolutePath(file_path);
    std::error_code file_error;
    if (!std::filesystem::exists(absolute_file_path, file_error)) {
        return unavailable;
    }

    const std::filesystem::path git_root = FindGitRoot(absolute_file_path);
    if (git_root.empty()) {
        return unavailable;
    }

    const std::filesystem::path relative_path = RelativePathForGit(absolute_file_path, git_root);
    const std::string git_root_arg = ShellQuote(git_root.string());
    const std::string file_arg = ShellQuote(relative_path.generic_string());

    int exit_code = 0;
    const std::string untracked = RunCommand("git -C " + git_root_arg +
                                                " ls-files --others --exclude-standard -- " + file_arg +
                                                " 2>/dev/null",
                                            &exit_code);
    if (exit_code == 0 && HasAnyOutput(untracked)) {
        GitLineChange added_line;
        added_line.marker = GitLineMarker::Added;
        return {
            .available = true,
            .lines = std::vector<GitLineChange>(line_count, added_line),
        };
    }

    const std::string diff = RunCommand("git -C " + git_root_arg + " diff HEAD --unified=0 -- " + file_arg +
                                            " 2>/dev/null",
                                        &exit_code);
    if (exit_code != 0) {
        return unavailable;
    }

    return ParseGitDiffMarkers(diff, line_count);
}

}  // namespace patchwork
