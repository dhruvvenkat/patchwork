#include "syntax/cpp_highlighter.h"

#include <array>
#include <cctype>
#include <string_view>

namespace flowstate {

namespace {

constexpr uint64_t kInsideBlockCommentMask = 1;

constexpr std::array<std::string_view, 66> kKeywords = {
    "alignas",       "alignof",      "asm",          "break",        "case",          "catch",
    "class",         "co_await",     "co_return",    "co_yield",     "const",         "const_cast",
    "consteval",     "constexpr",    "constinit",    "continue",     "decltype",      "default",
    "delete",        "do",           "dynamic_cast", "else",         "enum",          "explicit",
    "export",        "extern",       "final",        "for",          "friend",        "goto",
    "if",            "import",       "inline",       "module",       "mutable",       "namespace",
    "new",           "noexcept",     "operator",     "override",     "private",       "protected",
    "public",        "register",     "reinterpret_cast", "requires", "return",        "sizeof",
    "static",        "static_assert","static_cast",  "struct",       "switch",        "template",
    "this",          "thread_local", "throw",        "try",          "typedef",       "typeid",
    "typename",      "union",        "using",        "virtual",      "volatile",      "while",
};

constexpr std::array<std::string_view, 33> kTypeKeywords = {
    "auto",      "bool",       "char",      "char8_t",   "char16_t", "char32_t",
    "double",    "float",      "int",       "long",      "short",    "signed",
    "unsigned",  "void",       "wchar_t",   "size_t",    "ssize_t",  "ptrdiff_t",
    "intptr_t",  "uintptr_t",  "int8_t",    "int16_t",   "int32_t",  "int64_t",
    "uint8_t",   "uint16_t",   "uint32_t",  "uint64_t",  "FILE",     "clock_t",
    "time_t",    "std::size_t","std::nullptr_t",
};

constexpr std::array<std::string_view, 3> kLiteralKeywords = {
    "false",
    "nullptr",
    "true",
};

constexpr std::array<std::string_view, 5> kTypeDeclarators = {
    "class",
    "concept",
    "enum",
    "struct",
    "union",
};

template <size_t N>
bool ContainsWord(const std::array<std::string_view, N>& words, std::string_view token) {
    for (std::string_view word : words) {
        if (word == token) {
            return true;
        }
    }
    return false;
}

bool IsIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool IsIdentifierBody(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

size_t SkipWhitespace(std::string_view line, size_t index) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
        ++index;
    }
    return index;
}

size_t ScanIdentifier(std::string_view line, size_t index) {
    while (index < line.size() && IsIdentifierBody(line[index])) {
        ++index;
    }
    return index;
}

size_t ScanEscapedQuotedLiteral(std::string_view line, size_t index, char quote) {
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

size_t ScanRawStringLiteral(std::string_view line, size_t index, size_t raw_quote_index) {
    const size_t delimiter_start = raw_quote_index + 1;
    const size_t open_paren = line.find('(', delimiter_start);
    if (open_paren == std::string_view::npos) {
        return line.size();
    }

    const std::string_view delimiter = line.substr(delimiter_start, open_paren - delimiter_start);
    size_t search_index = open_paren + 1;
    while (true) {
        const size_t close_paren = line.find(')', search_index);
        if (close_paren == std::string_view::npos) {
            return line.size();
        }
        if (line.compare(close_paren + 1, delimiter.size(), delimiter) == 0 &&
            close_paren + 1 + delimiter.size() < line.size() &&
            line[close_paren + 1 + delimiter.size()] == '"') {
            return close_paren + 2 + delimiter.size();
        }
        search_index = close_paren + 1;
    }
}

bool MatchStringPrefix(std::string_view line,
                       size_t index,
                       char* quote,
                       size_t* quote_index,
                       bool* raw_string) {
    if (quote == nullptr || quote_index == nullptr || raw_string == nullptr) {
        return false;
    }

    auto set_plain_literal = [&](size_t prefix_length, char quote_char) {
        *quote = quote_char;
        *quote_index = index + prefix_length;
        *raw_string = false;
        return true;
    };

    auto set_raw_literal = [&](size_t prefix_length) {
        *quote = '"';
        *quote_index = index + prefix_length;
        *raw_string = true;
        return true;
    };

    if (index >= line.size()) {
        return false;
    }

    if (line[index] == '"' || line[index] == '\'') {
        return set_plain_literal(0, line[index]);
    }

    if (line.compare(index, 3, "u8R") == 0 && index + 3 < line.size() && line[index + 3] == '"') {
        return set_raw_literal(3);
    }
    if (line.compare(index, 2, "u8") == 0 && index + 2 < line.size() &&
        (line[index + 2] == '"' || line[index + 2] == '\'')) {
        return set_plain_literal(2, line[index + 2]);
    }
    if ((line[index] == 'u' || line[index] == 'U' || line[index] == 'L') && index + 2 < line.size() &&
        line[index + 1] == 'R' && line[index + 2] == '"') {
        return set_raw_literal(2);
    }
    if (line[index] == 'R' && index + 1 < line.size() && line[index + 1] == '"') {
        return set_raw_literal(1);
    }
    if ((line[index] == 'u' || line[index] == 'U' || line[index] == 'L') && index + 1 < line.size() &&
        (line[index + 1] == '"' || line[index + 1] == '\'')) {
        return set_plain_literal(1, line[index + 1]);
    }

    return false;
}

size_t ScanStringLiteral(std::string_view line, size_t index) {
    char quote = '\0';
    size_t quote_index = 0;
    bool raw_string = false;
    if (!MatchStringPrefix(line, index, &quote, &quote_index, &raw_string)) {
        return index;
    }

    if (raw_string) {
        return ScanRawStringLiteral(line, index, quote_index);
    }

    return ScanEscapedQuotedLiteral(line, quote_index, quote);
}

bool IsNumberStart(std::string_view line, size_t index) {
    if (index >= line.size()) {
        return false;
    }

    if (!std::isdigit(static_cast<unsigned char>(line[index]))) {
        return false;
    }

    if (index > 0) {
        const char previous = line[index - 1];
        if (IsIdentifierBody(previous) || previous == '.') {
            return false;
        }
    }

    return true;
}

size_t ScanDecimalDigits(std::string_view line, size_t index) {
    while (index < line.size() &&
           (std::isdigit(static_cast<unsigned char>(line[index])) || line[index] == '\'')) {
        ++index;
    }
    return index;
}

size_t ScanHexDigits(std::string_view line, size_t index) {
    while (index < line.size() &&
           (std::isxdigit(static_cast<unsigned char>(line[index])) || line[index] == '\'')) {
        ++index;
    }
    return index;
}

size_t ScanBinaryDigits(std::string_view line, size_t index) {
    while (index < line.size() &&
           (line[index] == '0' || line[index] == '1' || line[index] == '\'')) {
        ++index;
    }
    return index;
}

size_t ScanNumericSuffix(std::string_view line, size_t index) {
    while (index < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[index])) || line[index] == '_')) {
        ++index;
    }
    return index;
}

size_t ScanNumberLiteral(std::string_view line, size_t index) {
    if (index + 1 < line.size() && line[index] == '0' && (line[index + 1] == 'x' || line[index + 1] == 'X')) {
        index += 2;
        index = ScanHexDigits(line, index);
        if (index < line.size() && line[index] == '.') {
            ++index;
            index = ScanHexDigits(line, index);
        }
        if (index < line.size() && (line[index] == 'p' || line[index] == 'P')) {
            ++index;
            if (index < line.size() && (line[index] == '+' || line[index] == '-')) {
                ++index;
            }
            index = ScanDecimalDigits(line, index);
        }
        return ScanNumericSuffix(line, index);
    }

    if (index + 1 < line.size() && line[index] == '0' && (line[index + 1] == 'b' || line[index + 1] == 'B')) {
        index += 2;
        index = ScanBinaryDigits(line, index);
        return ScanNumericSuffix(line, index);
    }

    index = ScanDecimalDigits(line, index);
    if (index < line.size() && line[index] == '.') {
        ++index;
        index = ScanDecimalDigits(line, index);
    }
    if (index < line.size() && (line[index] == 'e' || line[index] == 'E')) {
        ++index;
        if (index < line.size() && (line[index] == '+' || line[index] == '-')) {
            ++index;
        }
        index = ScanDecimalDigits(line, index);
    }
    return ScanNumericSuffix(line, index);
}

bool IsFunctionIdentifier(std::string_view line, size_t identifier_end) {
    const size_t next = SkipWhitespace(line, identifier_end);
    return next < line.size() && line[next] == '(';
}

void AppendSpan(std::vector<SyntaxSpan>* spans, size_t start, size_t end, SyntaxTokenKind kind) {
    if (spans == nullptr || start >= end) {
        return;
    }
    spans->push_back({.start = start, .end = end, .kind = kind});
}

struct PreprocessorInfo {
    bool active = false;
    std::string_view directive;
};

PreprocessorInfo AppendPreprocessorDirectiveSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    PreprocessorInfo info;
    if (spans == nullptr) {
        return info;
    }

    const size_t directive_start = line.find_first_not_of(" \t");
    if (directive_start == std::string_view::npos || line[directive_start] != '#') {
        return info;
    }

    size_t directive_name_start = SkipWhitespace(line, directive_start + 1);
    if (directive_name_start >= line.size() || !IsIdentifierStart(line[directive_name_start])) {
        return info;
    }

    const size_t directive_end = ScanIdentifier(line, directive_name_start);
    info.active = true;
    info.directive = line.substr(directive_name_start, directive_end - directive_name_start);
    AppendSpan(spans, directive_start, directive_end, SyntaxTokenKind::Preprocessor);

    if (info.directive == "include" || info.directive == "include_next") {
        const size_t header_start = SkipWhitespace(line, directive_end);
        if (header_start >= line.size()) {
            return info;
        }

        char closing_delimiter = '\0';
        if (line[header_start] == '<') {
            closing_delimiter = '>';
        } else if (line[header_start] == '"') {
            closing_delimiter = '"';
        } else {
            return info;
        }

        size_t header_end = line.find(closing_delimiter, header_start + 1);
        if (header_end == std::string_view::npos) {
            header_end = line.size();
        } else {
            ++header_end;
        }
        AppendSpan(spans, header_start, header_end, SyntaxTokenKind::IncludePath);
        return info;
    }

    if (info.directive == "define" || info.directive == "ifdef" || info.directive == "ifndef" ||
        info.directive == "undef") {
        const size_t macro_start = SkipWhitespace(line, directive_end);
        if (macro_start < line.size() && IsIdentifierStart(line[macro_start])) {
            AppendSpan(spans, macro_start, ScanIdentifier(line, macro_start), SyntaxTokenKind::Macro);
        }
    }

    return info;
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
            index = ScanEscapedQuotedLiteral(line, index, line[index]);
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

bool ProtectedSpanEnd(const std::vector<SyntaxSpan>& spans, size_t index, size_t* span_end) {
    if (span_end == nullptr) {
        return false;
    }

    for (const SyntaxSpan& span : spans) {
        if (index >= span.start && index < span.end) {
            *span_end = span.end;
            return true;
        }
    }

    return false;
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    if (spans == nullptr) {
        return;
    }

    size_t index = 0;
    bool expect_type_name = false;
    while (index < line.size()) {
        size_t protected_end = 0;
        if (ProtectedSpanEnd(*spans, index, &protected_end)) {
            index = protected_end;
            continue;
        }

        if (line[index] == '"' || line[index] == '\'' || line[index] == 'u' || line[index] == 'U' ||
            line[index] == 'L' || line[index] == 'R') {
            const size_t literal_end = ScanStringLiteral(line, index);
            if (literal_end > index) {
                AppendSpan(spans, index, literal_end, SyntaxTokenKind::String);
                index = literal_end;
                expect_type_name = false;
                continue;
            }
        }

        if (IsNumberStart(line, index)) {
            const size_t number_end = ScanNumberLiteral(line, index);
            AppendSpan(spans, index, number_end, SyntaxTokenKind::Number);
            index = number_end;
            expect_type_name = false;
            continue;
        }

        if (IsIdentifierStart(line[index])) {
            const size_t identifier_end = ScanIdentifier(line, index);
            const std::string_view token = line.substr(index, identifier_end - index);

            if (ContainsWord(kTypeDeclarators, token)) {
                AppendSpan(spans, index, identifier_end, SyntaxTokenKind::Keyword);
                expect_type_name = true;
            } else if (ContainsWord(kKeywords, token) || ContainsWord(kLiteralKeywords, token)) {
                AppendSpan(spans, index, identifier_end, SyntaxTokenKind::Keyword);
                if (token == "using") {
                    expect_type_name = true;
                } else if (token != "class" && token != "struct") {
                    expect_type_name = false;
                }
            } else if (ContainsWord(kTypeKeywords, token)) {
                AppendSpan(spans, index, identifier_end, SyntaxTokenKind::Type);
                expect_type_name = false;
            } else if (expect_type_name) {
                AppendSpan(spans, index, identifier_end, SyntaxTokenKind::Type);
                expect_type_name = false;
            } else if (IsFunctionIdentifier(line, identifier_end)) {
                AppendSpan(spans, index, identifier_end, SyntaxTokenKind::Function);
            }

            index = identifier_end;
            continue;
        }

        if (!std::isspace(static_cast<unsigned char>(line[index])) && line[index] != ':' && line[index] != '<') {
            expect_type_name = false;
        }
        ++index;
    }
}

}  // namespace

LanguageId CppHighlighter::language() const { return LanguageId::Cpp; }

SyntaxLineState CppHighlighter::HighlightLine(std::string_view line,
                                              SyntaxLineState in_state,
                                              std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
        AppendPreprocessorDirectiveSpans(line, spans);
    }

    const bool starts_in_block_comment = (in_state.value & kInsideBlockCommentMask) != 0;
    const bool ends_in_block_comment = AppendCommentSpans(line, starts_in_block_comment, spans);
    if (spans != nullptr) {
        AppendLexicalSpans(line, spans);
    }
    return {.value = ends_in_block_comment ? kInsideBlockCommentMask : 0};
}

}  // namespace flowstate
