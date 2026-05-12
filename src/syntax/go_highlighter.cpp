#include "syntax/go_highlighter.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace flowstate {

namespace {

enum class GoScanMode : uint64_t {
    None = 0,
    BlockComment = 1,
    SingleQuote = 2,
    DoubleQuote = 3,
    RawString = 4,
};

constexpr auto kKeywords = std::to_array<std::string_view>({
    "break",    "case",    "chan",   "const",   "continue", "default", "defer",
    "else",     "fallthrough", "for", "func",   "go",       "goto",    "if",
    "import",   "interface", "map",   "package", "range",    "return",  "select",
    "struct",   "switch",   "type",   "var",
});

constexpr auto kTypeKeywords = std::to_array<std::string_view>({
    "any",       "bool",      "byte",      "comparable", "complex64", "complex128",
    "error",     "float32",   "float64",   "int",        "int8",      "int16",
    "int32",     "int64",     "rune",      "string",     "uint",      "uint8",
    "uint16",    "uint32",    "uint64",    "uintptr",
});

template <size_t N>
bool ContainsWord(const std::array<std::string_view, N>& words, std::string_view token) {
    for (std::string_view word : words) {
        if (word == token) {
            return true;
        }
    }
    return false;
}

SyntaxLineState EncodeState(GoScanMode mode) {
    return {.value = static_cast<uint64_t>(mode)};
}

GoScanMode DecodeMode(SyntaxLineState state) {
    return static_cast<GoScanMode>(state.value);
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

void AppendSpan(std::vector<SyntaxSpan>* spans, size_t start, size_t end, SyntaxTokenKind kind) {
    if (spans == nullptr || start >= end) {
        return;
    }
    spans->push_back({.start = start, .end = end, .kind = kind});
}

bool SpanEndAt(const std::vector<SyntaxSpan>& spans, size_t index, size_t* end) {
    if (end == nullptr) {
        return false;
    }
    for (const SyntaxSpan& span : spans) {
        if (index >= span.start && index < span.end) {
            *end = span.end;
            return true;
        }
    }
    return false;
}

bool LooksLikeTypeName(std::string_view token) {
    return !token.empty() && std::isupper(static_cast<unsigned char>(token.front())) != 0;
}

std::optional<size_t> FindEscapedQuoteEnd(std::string_view line, size_t search_index, char quote) {
    size_t index = search_index;
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
    return std::nullopt;
}

std::optional<size_t> FindBlockCommentEnd(std::string_view line, size_t search_index) {
    const size_t found = line.find("*/", search_index);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    return found + 2;
}

std::optional<size_t> FindRawStringEnd(std::string_view line, size_t search_index) {
    const size_t found = line.find('`', search_index);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    return found + 1;
}

bool IsNumberStart(std::string_view line, size_t index) {
    if (index >= line.size() || !std::isdigit(static_cast<unsigned char>(line[index]))) {
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

size_t ScanDigitsWithUnderscores(std::string_view line, size_t index, int base) {
    while (index < line.size()) {
        const char ch = line[index];
        if (ch == '_') {
            ++index;
            continue;
        }

        const bool valid =
            (base == 16 && std::isxdigit(static_cast<unsigned char>(ch))) ||
            (base == 10 && std::isdigit(static_cast<unsigned char>(ch))) ||
            (base == 8 && ch >= '0' && ch <= '7') ||
            (base == 2 && (ch == '0' || ch == '1'));
        if (!valid) {
            break;
        }
        ++index;
    }
    return index;
}

size_t ScanNumericSuffix(std::string_view line, size_t index) {
    if (index < line.size() && line[index] == 'i') {
        ++index;
    }
    return index;
}

size_t ScanNumberLiteral(std::string_view line, size_t index) {
    if (index + 1 < line.size() && line[index] == '0') {
        const char prefix = line[index + 1];
        if (prefix == 'x' || prefix == 'X') {
            index = ScanDigitsWithUnderscores(line, index + 2, 16);
            return ScanNumericSuffix(line, index);
        }
        if (prefix == 'b' || prefix == 'B') {
            index = ScanDigitsWithUnderscores(line, index + 2, 2);
            return ScanNumericSuffix(line, index);
        }
        if (prefix == 'o' || prefix == 'O') {
            index = ScanDigitsWithUnderscores(line, index + 2, 8);
            return ScanNumericSuffix(line, index);
        }
    }

    index = ScanDigitsWithUnderscores(line, index, 10);
    if (index < line.size() && line[index] == '.' && index + 1 < line.size() &&
        std::isdigit(static_cast<unsigned char>(line[index + 1]))) {
        ++index;
        index = ScanDigitsWithUnderscores(line, index, 10);
    }
    if (index < line.size() && (line[index] == 'e' || line[index] == 'E' || line[index] == 'p' || line[index] == 'P')) {
        ++index;
        if (index < line.size() && (line[index] == '+' || line[index] == '-')) {
            ++index;
        }
        index = ScanDigitsWithUnderscores(line, index, 10);
    }
    return ScanNumericSuffix(line, index);
}

bool IsFunctionCall(std::string_view line, size_t identifier_end) {
    const size_t next = SkipWhitespace(line, identifier_end);
    return next < line.size() && line[next] == '(';
}

SyntaxLineState AppendProtectedSpans(std::string_view line,
                                     SyntaxLineState in_state,
                                     std::vector<SyntaxSpan>* spans) {
    size_t index = 0;
    const GoScanMode active_mode = DecodeMode(in_state);
    if (active_mode != GoScanMode::None) {
        std::optional<size_t> end;
        switch (active_mode) {
            case GoScanMode::BlockComment:
                end = FindBlockCommentEnd(line, 0);
                break;
            case GoScanMode::SingleQuote:
                end = FindEscapedQuoteEnd(line, 0, '\'');
                break;
            case GoScanMode::DoubleQuote:
                end = FindEscapedQuoteEnd(line, 0, '"');
                break;
            case GoScanMode::RawString:
                end = FindRawStringEnd(line, 0);
                break;
            case GoScanMode::None:
                break;
        }

        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end,
                   active_mode == GoScanMode::BlockComment ? SyntaxTokenKind::Comment : SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(active_mode);
        }
        index = span_end;
    }

    while (index < line.size()) {
        if (line.compare(index, 2, "//") == 0) {
            AppendSpan(spans, index, line.size(), SyntaxTokenKind::Comment);
            return EncodeState(GoScanMode::None);
        }
        if (line.compare(index, 2, "/*") == 0) {
            const std::optional<size_t> end = FindBlockCommentEnd(line, index + 2);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::Comment);
            if (!end.has_value()) {
                return EncodeState(GoScanMode::BlockComment);
            }
            index = span_end;
            continue;
        }
        if (line[index] == '"') {
            const std::optional<size_t> end = FindEscapedQuoteEnd(line, index + 1, '"');
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(GoScanMode::DoubleQuote);
            }
            index = span_end;
            continue;
        }
        if (line[index] == '\'') {
            const std::optional<size_t> end = FindEscapedQuoteEnd(line, index + 1, '\'');
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(GoScanMode::SingleQuote);
            }
            index = span_end;
            continue;
        }
        if (line[index] == '`') {
            const std::optional<size_t> end = FindRawStringEnd(line, index + 1);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(GoScanMode::RawString);
            }
            index = span_end;
            continue;
        }
        ++index;
    }

    return EncodeState(GoScanMode::None);
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    enum class ExpectedName {
        None,
        Type,
        Function,
    };

    ExpectedName expected_name = ExpectedName::None;
    bool type_context = false;
    bool pending_receiver = false;
    int receiver_depth = 0;

    for (size_t index = 0; index < line.size();) {
        size_t protected_end = 0;
        if (spans != nullptr && SpanEndAt(*spans, index, &protected_end)) {
            index = protected_end;
            continue;
        }

        const char ch = line[index];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++index;
            continue;
        }

        if (pending_receiver) {
            if (ch == '(') {
                ++receiver_depth;
            } else if (ch == ')') {
                --receiver_depth;
                if (receiver_depth <= 0) {
                    pending_receiver = false;
                    expected_name = ExpectedName::Function;
                }
            }
            ++index;
            continue;
        }

        if (ch == ';' || ch == '{' || ch == '}' || ch == '=') {
            type_context = false;
            expected_name = ExpectedName::None;
            ++index;
            continue;
        }

        if (ch == ',' || ch == '(' || ch == '[' || ch == '*' || ch == '<' || ch == '|') {
            if (type_context || ch == '[' || ch == '*') {
                type_context = true;
            }
            ++index;
            continue;
        }

        if (ch == ')' || ch == ']') {
            ++index;
            continue;
        }

        if (IsNumberStart(line, index)) {
            const size_t end = ScanNumberLiteral(line, index);
            AppendSpan(spans, index, end, SyntaxTokenKind::Number);
            index = end;
            continue;
        }

        if (!IsIdentifierStart(ch)) {
            ++index;
            continue;
        }

        const size_t end = ScanIdentifier(line, index);
        const std::string_view token = line.substr(index, end - index);

        if (ContainsWord(kTypeKeywords, token)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            index = end;
            continue;
        }

        if (ContainsWord(kKeywords, token)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Keyword);
            if (token == "type") {
                expected_name = ExpectedName::Type;
                type_context = false;
            } else if (token == "func") {
                const size_t next = SkipWhitespace(line, end);
                if (next < line.size() && line[next] == '(') {
                    pending_receiver = true;
                    receiver_depth = 0;
                    expected_name = ExpectedName::None;
                } else {
                    expected_name = ExpectedName::Function;
                }
                type_context = false;
            } else if (token == "struct" || token == "interface" || token == "map" || token == "chan") {
                type_context = true;
                expected_name = ExpectedName::None;
            } else if (token == "var" || token == "const") {
                type_context = false;
                expected_name = ExpectedName::None;
            } else {
                expected_name = ExpectedName::None;
            }
            index = end;
            continue;
        }

        if (expected_name == ExpectedName::Type) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            expected_name = ExpectedName::None;
            index = end;
            continue;
        }

        if (expected_name == ExpectedName::Function) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Function);
            expected_name = ExpectedName::None;
            index = end;
            continue;
        }

        if (type_context && LooksLikeTypeName(token)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            index = end;
            continue;
        }

        if (IsFunctionCall(line, end)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Function);
            type_context = false;
            index = end;
            continue;
        }

        index = end;
    }
}

}  // namespace

LanguageId GoHighlighter::language() const { return LanguageId::Go; }

SyntaxLineState GoHighlighter::HighlightLine(std::string_view line,
                                             SyntaxLineState in_state,
                                             std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    const SyntaxLineState next_state = AppendProtectedSpans(line, in_state, spans);
    AppendLexicalSpans(line, spans);
    return next_state;
}

}  // namespace flowstate
