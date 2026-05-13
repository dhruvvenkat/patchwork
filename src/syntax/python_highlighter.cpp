#include "syntax/python_highlighter.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace flowstate {

namespace {

enum class PythonScanMode : uint64_t {
    None = 0,
    SingleQuote = 1,
    DoubleQuote = 2,
    TripleSingleQuote = 3,
    TripleDoubleQuote = 4,
};

constexpr std::array<std::string_view, 37> kKeywords = {
    "and",   "as",     "assert", "async", "await",  "break",  "case",   "class", "continue",
    "def",   "del",    "elif",   "else",  "except", "finally","for",    "from",  "global",
    "if",    "import", "in",     "is",    "lambda", "match",  "nonlocal","not",   "or",
    "pass",  "raise",  "return", "try",   "while",  "with",   "yield",  "None",
};

constexpr std::array<std::string_view, 2> kLiteralKeywords = {
    "False",
    "True",
};

constexpr std::array<std::string_view, 11> kTypeKeywords = {
    "bool", "bytes", "bytearray", "complex", "dict", "float", "frozenset",
    "int",  "list",  "set",       "str",
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

SyntaxLineState EncodeState(PythonScanMode mode) {
    return {.value = static_cast<uint64_t>(mode)};
}

PythonScanMode DecodeMode(SyntaxLineState state) {
    return static_cast<PythonScanMode>(state.value);
}

bool IsIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool IsIdentifierBody(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool IsStringPrefixLetter(char ch) {
    switch (ch) {
        case 'r':
        case 'R':
        case 'b':
        case 'B':
        case 'u':
        case 'U':
        case 'f':
        case 'F':
            return true;
        default:
            return false;
    }
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

bool MatchStringStart(std::string_view line,
                      size_t index,
                      size_t* prefix_length,
                      char* quote_char,
                      bool* triple_quoted) {
    if (prefix_length == nullptr || quote_char == nullptr || triple_quoted == nullptr || index >= line.size()) {
        return false;
    }

    size_t quote_index = index;
    if (line[index] != '\'' && line[index] != '"') {
        if (!IsStringPrefixLetter(line[index])) {
            return false;
        }

        size_t cursor = index;
        while (cursor < line.size() && cursor - index < 2 && IsStringPrefixLetter(line[cursor])) {
            ++cursor;
        }
        if (cursor == index || cursor >= line.size() || (line[cursor] != '\'' && line[cursor] != '"')) {
            return false;
        }
        quote_index = cursor;
    }

    *prefix_length = quote_index - index;
    *quote_char = line[quote_index];
    *triple_quoted = quote_index + 2 < line.size() && line[quote_index + 1] == *quote_char &&
                     line[quote_index + 2] == *quote_char;
    return true;
}

std::optional<size_t> FindQuotedStringEnd(std::string_view line, size_t search_index, char quote_char) {
    size_t index = search_index;
    while (index < line.size()) {
        if (line[index] == '\\' && index + 1 < line.size()) {
            index += 2;
            continue;
        }
        if (line[index] == quote_char) {
            return index + 1;
        }
        ++index;
    }
    return std::nullopt;
}

std::optional<size_t> FindTripleQuotedStringEnd(std::string_view line, size_t search_index, char quote_char) {
    for (size_t index = search_index; index + 2 < line.size(); ++index) {
        if (line[index] == quote_char && line[index + 1] == quote_char && line[index + 2] == quote_char) {
            return index + 3;
        }
    }
    return std::nullopt;
}

PythonScanMode ModeForQuote(char quote_char, bool triple_quoted) {
    if (quote_char == '\'') {
        return triple_quoted ? PythonScanMode::TripleSingleQuote : PythonScanMode::SingleQuote;
    }
    return triple_quoted ? PythonScanMode::TripleDoubleQuote : PythonScanMode::DoubleQuote;
}

bool IsTripleQuotedMode(PythonScanMode mode) {
    return mode == PythonScanMode::TripleSingleQuote || mode == PythonScanMode::TripleDoubleQuote;
}

char QuoteCharForMode(PythonScanMode mode) {
    switch (mode) {
        case PythonScanMode::SingleQuote:
        case PythonScanMode::TripleSingleQuote:
            return '\'';
        case PythonScanMode::DoubleQuote:
        case PythonScanMode::TripleDoubleQuote:
            return '"';
        case PythonScanMode::None:
            return '"';
    }
    return '"';
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
    while (index < line.size() && (line[index] == 'j' || line[index] == 'J')) {
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
        if (prefix == 'o' || prefix == 'O') {
            index = ScanDigitsWithUnderscores(line, index + 2, 8);
            return ScanNumericSuffix(line, index);
        }
        if (prefix == 'b' || prefix == 'B') {
            index = ScanDigitsWithUnderscores(line, index + 2, 2);
            return ScanNumericSuffix(line, index);
        }
    }

    index = ScanDigitsWithUnderscores(line, index, 10);
    if (index < line.size() && line[index] == '.' && index + 1 < line.size() &&
        std::isdigit(static_cast<unsigned char>(line[index + 1]))) {
        ++index;
        index = ScanDigitsWithUnderscores(line, index, 10);
    }
    if (index < line.size() && (line[index] == 'e' || line[index] == 'E')) {
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

bool LooksLikeTypeName(std::string_view token) {
    return !token.empty() && std::isupper(static_cast<unsigned char>(token.front())) != 0;
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

SyntaxLineState AppendProtectedSpans(std::string_view line,
                                     SyntaxLineState in_state,
                                     std::vector<SyntaxSpan>* spans) {
    size_t index = 0;
    const PythonScanMode active_mode = DecodeMode(in_state);
    if (active_mode != PythonScanMode::None) {
        const char quote_char = QuoteCharForMode(active_mode);
        const std::optional<size_t> end = IsTripleQuotedMode(active_mode)
                                              ? FindTripleQuotedStringEnd(line, 0, quote_char)
                                              : FindQuotedStringEnd(line, 0, quote_char);
        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end, SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(active_mode);
        }
        index = span_end;
    }

    while (index < line.size()) {
        if (line[index] == '#') {
            AppendSpan(spans, index, line.size(), SyntaxTokenKind::Comment);
            return EncodeState(PythonScanMode::None);
        }

        size_t prefix_length = 0;
        char quote_char = '\0';
        bool triple_quoted = false;
        if (MatchStringStart(line, index, &prefix_length, &quote_char, &triple_quoted)) {
            const size_t quote_index = index + prefix_length;
            const size_t search_index = quote_index + (triple_quoted ? 3 : 1);
            const std::optional<size_t> end = triple_quoted ? FindTripleQuotedStringEnd(line, search_index, quote_char)
                                                            : FindQuotedStringEnd(line, search_index, quote_char);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(ModeForQuote(quote_char, triple_quoted));
            }
            index = span_end;
            continue;
        }

        ++index;
    }

    return EncodeState(PythonScanMode::None);
}

void AppendDecoratorSpan(std::string_view line, size_t index, std::vector<SyntaxSpan>* spans) {
    if (spans == nullptr || index >= line.size() || line[index] != '@') {
        return;
    }

    size_t cursor = index + 1;
    std::optional<size_t> identifier_start;
    while (cursor < line.size()) {
        if (!identifier_start.has_value()) {
            if (!IsIdentifierStart(line[cursor])) {
                break;
            }
            identifier_start = cursor;
            cursor = ScanIdentifier(line, cursor);
            continue;
        }

        if (line[cursor] != '.') {
            break;
        }
        ++cursor;
        if (cursor >= line.size() || !IsIdentifierStart(line[cursor])) {
            break;
        }
        cursor = ScanIdentifier(line, cursor);
    }

    if (identifier_start.has_value()) {
        AppendSpan(spans, index, cursor, SyntaxTokenKind::Macro);
    }
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    if (spans == nullptr) {
        return;
    }

    size_t index = 0;
    bool expect_function_name = false;
    bool expect_type_name = false;
    while (index < line.size()) {
        size_t protected_end = 0;
        if (SpanEndAt(*spans, index, &protected_end)) {
            index = protected_end;
            expect_function_name = false;
            continue;
        }

        if (line[index] == '@') {
            AppendDecoratorSpan(line, index, spans);
            size_t decorator_end = index;
            if (SpanEndAt(*spans, index, &decorator_end)) {
                index = decorator_end;
                continue;
            }
        }

        if (IsNumberStart(line, index)) {
            const size_t end = ScanNumberLiteral(line, index);
            AppendSpan(spans, index, end, SyntaxTokenKind::Number);
            index = end;
            continue;
        }

        if (IsIdentifierStart(line[index])) {
            const size_t end = ScanIdentifier(line, index);
            const std::string_view token = line.substr(index, end - index);

            if (expect_function_name) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Function);
                expect_function_name = false;
                expect_type_name = false;
            } else if (expect_type_name) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Type);
                expect_type_name = false;
            } else if (ContainsWord(kKeywords, token) || ContainsWord(kLiteralKeywords, token)) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Keyword);
                expect_function_name = token == "def";
                expect_type_name = token == "class";
            } else if (ContainsWord(kTypeKeywords, token) || LooksLikeTypeName(token)) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            } else if (IsFunctionCall(line, end)) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Function);
            }

            index = end;
            continue;
        }

        ++index;
    }
}

}  // namespace

LanguageId PythonHighlighter::language() const { return LanguageId::Python; }

SyntaxLineState PythonHighlighter::HighlightLine(std::string_view line,
                                                 SyntaxLineState in_state,
                                                 std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    const SyntaxLineState next_state = AppendProtectedSpans(line, in_state, spans);
    if (spans != nullptr) {
        AppendLexicalSpans(line, spans);
    }
    return next_state;
}

}  // namespace flowstate
