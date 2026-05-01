#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace patchwork {

enum class GitLineMarker {
    Clean,
    Added,
    Modified,
    Deleted,
};

struct GitLineStatus {
    bool available = false;
    std::vector<GitLineMarker> markers;
};

GitLineStatus ParseGitDiffMarkers(std::string_view diff_text, size_t line_count);
GitLineStatus LoadGitLineStatus(const std::filesystem::path& file_path, size_t line_count);

}  // namespace patchwork
