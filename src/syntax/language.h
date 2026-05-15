#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace flowstate {

enum class LanguageId {
    PlainText,
    C,
    CHeader,
    Cpp,
    Rust,
    Python,
    JavaScript,
    TypeScript,
    Java,
    Go,
    Markdown,
};

LanguageId DetectLanguageId(const std::optional<std::filesystem::path>& path);
std::string_view LanguageDisplayName(LanguageId language_id);
std::optional<std::string_view> LineCommentPrefix(LanguageId language_id);

}  // namespace flowstate
