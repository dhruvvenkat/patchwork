#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace flowstate {

enum class DiffLineType {
    Context,
    Add,
    Remove,
};

struct DiffLine {
    DiffLineType type = DiffLineType::Context;
    std::string text;
};

struct Hunk {
    size_t old_start = 0;
    size_t old_count = 0;
    size_t new_start = 0;
    size_t new_count = 0;
    std::vector<DiffLine> lines;
};

struct PatchSet {
    std::string old_file;
    std::string new_file;
    std::vector<Hunk> hunks;
    std::vector<std::string> errors;

    bool valid() const;
    std::string targetFile() const;
};

std::string ExtractDiffText(std::string_view raw_text);
PatchSet ParseUnifiedDiff(std::string_view diff_text);

}  // namespace flowstate

