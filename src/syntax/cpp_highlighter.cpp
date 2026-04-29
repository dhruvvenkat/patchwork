#include "syntax/cpp_highlighter.h"

#include <cctype>
#include <string_view>

namespace patchwork {

namespace {

constexpr uint64_t kInsideBlockCommentMask = 1;

size_t SkipQuotedLiteral(std::string_view line, size_t index) {
    const char quote = line[index];
    ++index;
    while (index < line.size()) {
        if (line[index] == '\\' && index + 1 < line.size()) {
            index += 2;
            continue;
        }
        if (line[index] == quote) {
            return index + 1;
        }
        ++index;
    }
    return line.size();
}

void AppendIncludeSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    if (spans == nullptr) {
        return;
    }

    const size_t directive_start = line.find_first_not_of(" \t");
    if (directive_start == std::string_view::npos || line[directive_start] != '#') {
        return;
    }

    constexpr std::string_view kIncludeDirective = "#include";
    if (line.compare(directive_start, kIncludeDirective.size(), kIncludeDirective) != 0) {
        return;
    }

    const size_t directive_end = directive_start + kIncludeDirective.size();
    if (directive_end < line.size() && !std::isspace(static_cast<unsigned char>(line[directive_end]))) {
        return;
    }

    spans->push_back({.start = directive_start, .end = directive_end, .kind = SyntaxTokenKind::Preprocessor});

    const size_t header_start = line.find_first_not_of(" \t", directive_end);
    if (header_start == std::string_view::npos) {
        return;
    }

    char closing_delimiter = '\0';
    if (line[header_start] == '<') {
        closing_delimiter = '>';
    } else if (line[header_start] == '"') {
        closing_delimiter = '"';
    } else {
        return;
    }

    size_t header_end = line.find(closing_delimiter, header_start + 1);
    if (header_end == std::string_view::npos) {
        header_end = line.size();
    } else {
        ++header_end;
    }

    spans->push_back({.start = header_start, .end = header_end, .kind = SyntaxTokenKind::IncludePath});
}

bool AppendCommentSpans(std::string_view line,
                        bool starts_in_block_comment,
                        std::vector<SyntaxSpan>* spans) {
    size_t index = 0;
    bool in_block_comment = starts_in_block_comment;

    while (index < line.size()) {
        if (in_block_comment) {
            const size_t comment_end = line.find("*/", index);
            const size_t span_end = comment_end == std::string_view::npos ? line.size() : comment_end + 2;
            if (spans != nullptr) {
                spans->push_back({.start = index, .end = span_end, .kind = SyntaxTokenKind::Comment});
            }
            if (comment_end == std::string_view::npos) {
                return true;
            }
            index = span_end;
            in_block_comment = false;
            continue;
        }

        if (line[index] == '"' || line[index] == '\'') {
            index = SkipQuotedLiteral(line, index);
            continue;
        }

        if (index + 1 >= line.size()) {
            break;
        }

        if (line[index] == '/' && line[index + 1] == '/') {
            if (spans != nullptr) {
                spans->push_back({.start = index, .end = line.size(), .kind = SyntaxTokenKind::Comment});
            }
            return false;
        }

        if (line[index] == '/' && line[index + 1] == '*') {
            const size_t comment_end = line.find("*/", index + 2);
            const size_t span_end = comment_end == std::string_view::npos ? line.size() : comment_end + 2;
            if (spans != nullptr) {
                spans->push_back({.start = index, .end = span_end, .kind = SyntaxTokenKind::Comment});
            }
            if (comment_end == std::string_view::npos) {
                return true;
            }
            index = span_end;
            continue;
        }

        ++index;
    }

    return false;
}

}  // namespace

LanguageId CppHighlighter::language() const { return LanguageId::Cpp; }

SyntaxLineState CppHighlighter::HighlightLine(std::string_view line,
                                              SyntaxLineState in_state,
                                              std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
        AppendIncludeSpans(line, spans);
    }

    const bool starts_in_block_comment = (in_state.value & kInsideBlockCommentMask) != 0;
    const bool ends_in_block_comment = AppendCommentSpans(line, starts_in_block_comment, spans);
    return {.value = ends_in_block_comment ? kInsideBlockCommentMask : 0};
}

}  // namespace patchwork
