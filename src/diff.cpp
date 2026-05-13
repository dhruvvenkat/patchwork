#include "diff.h"

#include <cctype>
#include <regex>
#include <sstream>

namespace flowstate {

namespace {

std::string TrimPathPrefix(std::string path) {
    if (path.rfind("a/", 0) == 0 || path.rfind("b/", 0) == 0) {
        return path.substr(2);
    }
    return path;
}

size_t ParseCount(const std::string& value) {
    if (value.empty()) {
        return 1;
    }
    return static_cast<size_t>(std::stoul(value));
}

}  // namespace

bool PatchSet::valid() const { return !hunks.empty() && errors.empty(); }

std::string PatchSet::targetFile() const {
    if (!new_file.empty() && new_file != "/dev/null") {
        return new_file;
    }
    return old_file;
}

std::string ExtractDiffText(std::string_view raw_text) {
    const size_t diff_start = raw_text.find("--- ");
    if (diff_start == std::string_view::npos) {
        return {};
    }

    std::istringstream input{std::string(raw_text.substr(diff_start))};
    std::ostringstream output;
    std::string line;
    bool wrote_any = false;
    bool saw_hunk = false;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const bool is_diff_line = line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0 ||
                                  line.rfind("@@", 0) == 0 || line.rfind("diff --git", 0) == 0 ||
                                  line.rfind("\\", 0) == 0 ||
                                  (!line.empty() &&
                                   (line[0] == ' ' || line[0] == '+' || line[0] == '-'));

        if (!is_diff_line) {
            if (saw_hunk) {
                break;
            }
            continue;
        }

        saw_hunk = saw_hunk || line.rfind("@@", 0) == 0;
        if (wrote_any) {
            output << '\n';
        }
        output << line;
        wrote_any = true;
    }

    return output.str();
}

PatchSet ParseUnifiedDiff(std::string_view diff_text) {
    PatchSet patch;
    std::istringstream input{std::string(diff_text)};
    std::string line;
    Hunk* current_hunk = nullptr;
    const std::regex hunk_header(R"(^@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@)");

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("--- ", 0) == 0) {
            patch.old_file = TrimPathPrefix(line.substr(4));
            continue;
        }
        if (line.rfind("+++ ", 0) == 0) {
            patch.new_file = TrimPathPrefix(line.substr(4));
            continue;
        }

        std::smatch match;
        if (std::regex_search(line, match, hunk_header)) {
            Hunk hunk;
            hunk.old_start = static_cast<size_t>(std::stoul(match[1].str()));
            hunk.old_count = ParseCount(match[2].str());
            hunk.new_start = static_cast<size_t>(std::stoul(match[3].str()));
            hunk.new_count = ParseCount(match[4].str());
            patch.hunks.push_back(std::move(hunk));
            current_hunk = &patch.hunks.back();
            continue;
        }

        if (current_hunk == nullptr) {
            if (!line.empty() && line.rfind("diff --git", 0) != 0) {
                patch.errors.push_back("Unexpected text before first hunk.");
            }
            continue;
        }

        if (line.empty()) {
            current_hunk->lines.push_back({DiffLineType::Context, ""});
            continue;
        }

        const char prefix = line.front();
        const std::string body = line.substr(1);
        if (prefix == ' ') {
            current_hunk->lines.push_back({DiffLineType::Context, body});
        } else if (prefix == '+') {
            current_hunk->lines.push_back({DiffLineType::Add, body});
        } else if (prefix == '-') {
            current_hunk->lines.push_back({DiffLineType::Remove, body});
        } else if (prefix == '\\') {
            continue;
        } else {
            patch.errors.push_back("Invalid diff line: " + line);
        }
    }

    if (patch.hunks.empty()) {
        patch.errors.push_back("No hunks found.");
    }

    return patch;
}

}  // namespace flowstate
