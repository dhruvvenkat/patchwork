#include "syntax/language.h"

#include <string>
#include <unordered_map>

namespace flowstate {

LanguageId DetectLanguageId(const std::optional<std::filesystem::path>& path) {
    static const std::unordered_map<std::string, LanguageId> kExtensions = {
        {".c", LanguageId::C},          {".h", LanguageId::CHeader},      {".cc", LanguageId::Cpp},
        {".cpp", LanguageId::Cpp},      {".cxx", LanguageId::Cpp},        {".hpp", LanguageId::Cpp},
        {".rs", LanguageId::Rust},      {".py", LanguageId::Python},      {".js", LanguageId::JavaScript},
        {".ts", LanguageId::TypeScript}, {".java", LanguageId::Java},     {".go", LanguageId::Go},
        {".md", LanguageId::Markdown},  {".txt", LanguageId::PlainText},
    };

    if (!path.has_value()) {
        return LanguageId::PlainText;
    }

    const std::string extension = path->extension().string();
    auto found = kExtensions.find(extension);
    if (found != kExtensions.end()) {
        return found->second;
    }

    return LanguageId::PlainText;
}

std::string_view LanguageDisplayName(LanguageId language_id) {
    switch (language_id) {
        case LanguageId::PlainText:
            return "Text";
        case LanguageId::C:
            return "C";
        case LanguageId::CHeader:
            return "C/C++";
        case LanguageId::Cpp:
            return "C++";
        case LanguageId::Rust:
            return "Rust";
        case LanguageId::Python:
            return "Python";
        case LanguageId::JavaScript:
            return "JavaScript";
        case LanguageId::TypeScript:
            return "TypeScript";
        case LanguageId::Java:
            return "Java";
        case LanguageId::Go:
            return "Go";
        case LanguageId::Markdown:
            return "Markdown";
    }

    return "Text";
}

std::optional<std::string_view> LineCommentPrefix(LanguageId language_id) {
    switch (language_id) {
        case LanguageId::C:
        case LanguageId::CHeader:
        case LanguageId::Cpp:
        case LanguageId::Rust:
        case LanguageId::JavaScript:
        case LanguageId::TypeScript:
        case LanguageId::Java:
        case LanguageId::Go:
            return "//";
        case LanguageId::Python:
            return "#";
        case LanguageId::PlainText:
        case LanguageId::Markdown:
            return std::nullopt;
    }

    return std::nullopt;
}

}  // namespace flowstate
