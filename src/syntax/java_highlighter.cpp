#include "syntax/java_highlighter.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace patchwork {

namespace {

enum class JavaScanMode : uint64_t {
    None = 0,
    BlockComment = 1,
    SingleQuote = 2,
    DoubleQuote = 3,
    TextBlock = 4,
};

constexpr auto kKeywords = std::to_array<std::string_view>({
    "abstract",   "assert",       "break",      "case",         "catch",      "class",      "continue",
    "default",    "do",           "else",       "enum",         "extends",    "final",      "finally",
    "for",        "if",           "implements", "import",       "instanceof", "interface",  "native",
    "new",        "package",      "private",    "protected",    "public",     "record",     "return",
    "sealed",     "permits",      "static",     "strictfp",     "super",      "switch",     "synchronized",
    "this",       "throw",        "throws",     "transient",    "try",        "var",        "volatile",
    "while",      "yield",        "true",       "false",        "null",       "const",      "goto",
});

constexpr auto kTypeKeywords = std::to_array<std::string_view>({
    "boolean", "byte",   "char",   "double", "float",   "int",     "long",
    "short",   "void",   "String", "Object", "Boolean", "Byte",    "Character",
    "Double",  "Float",  "Integer","Long",   "Short",   "Number",  "Class",
    "Enum",    "Record", "List",   "Map",    "Set",     "Optional","RuntimeException",
    "Exception",
});

constexpr auto kModifierKeywords = std::to_array<std::string_view>({
    "abstract", "final",      "native",    "private", "protected", "public",
    "sealed",   "static",     "strictfp",  "synchronized",
    "transient","var",        "volatile",
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

SyntaxLineState EncodeState(JavaScanMode mode) {
    return {.value = static_cast<uint64_t>(mode)};
}

JavaScanMode DecodeMode(SyntaxLineState state) {
    return static_cast<JavaScanMode>(state.value);
}

bool IsIdentifierStart(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
}

bool IsIdentifierBody(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$';
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

std::optional<size_t> FindTextBlockEnd(std::string_view line, size_t search_index) {
    const size_t found = line.find("\"\"\"", search_index);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    return found + 3;
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
    while (index < line.size()) {
        const char ch = line[index];
        if (ch == 'l' || ch == 'L' || ch == 'f' || ch == 'F' || ch == 'd' || ch == 'D') {
            ++index;
            continue;
        }
        break;
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
        if (prefix >= '0' && prefix <= '7') {
            index = ScanDigitsWithUnderscores(line, index + 1, 8);
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

size_t ScanAnnotation(std::string_view line, size_t index) {
    size_t cursor = index + 1;
    if (cursor >= line.size() || !IsIdentifierStart(line[cursor])) {
        return cursor;
    }

    cursor = ScanIdentifier(line, cursor);
    while (cursor < line.size() && line[cursor] == '.') {
        const size_t segment_start = cursor + 1;
        if (segment_start >= line.size() || !IsIdentifierStart(line[segment_start])) {
            break;
        }
        cursor = ScanIdentifier(line, segment_start);
    }
    return cursor;
}

SyntaxLineState AppendProtectedSpans(std::string_view line,
                                     SyntaxLineState in_state,
                                     std::vector<SyntaxSpan>* spans) {
    size_t index = 0;
    const JavaScanMode active_mode = DecodeMode(in_state);
    if (active_mode != JavaScanMode::None) {
        std::optional<size_t> end;
        switch (active_mode) {
            case JavaScanMode::BlockComment:
                end = FindBlockCommentEnd(line, 0);
                break;
            case JavaScanMode::SingleQuote:
                end = FindEscapedQuoteEnd(line, 0, '\'');
                break;
            case JavaScanMode::DoubleQuote:
                end = FindEscapedQuoteEnd(line, 0, '"');
                break;
            case JavaScanMode::TextBlock:
                end = FindTextBlockEnd(line, 0);
                break;
            case JavaScanMode::None:
                break;
        }

        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end,
                   active_mode == JavaScanMode::BlockComment ? SyntaxTokenKind::Comment : SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(active_mode);
        }
        index = span_end;
    }

    while (index < line.size()) {
        if (line.compare(index, 2, "//") == 0) {
            AppendSpan(spans, index, line.size(), SyntaxTokenKind::Comment);
            return EncodeState(JavaScanMode::None);
        }
        if (line.compare(index, 2, "/*") == 0) {
            const std::optional<size_t> end = FindBlockCommentEnd(line, index + 2);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::Comment);
            if (!end.has_value()) {
                return EncodeState(JavaScanMode::BlockComment);
            }
            index = span_end;
            continue;
        }
        if (line.compare(index, 3, "\"\"\"") == 0) {
            const std::optional<size_t> end = FindTextBlockEnd(line, index + 3);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(JavaScanMode::TextBlock);
            }
            index = span_end;
            continue;
        }
        if (line[index] == '\'' || line[index] == '"') {
            const char quote = line[index];
            const std::optional<size_t> end = FindEscapedQuoteEnd(line, index + 1, quote);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(quote == '\'' ? JavaScanMode::SingleQuote : JavaScanMode::DoubleQuote);
            }
            index = span_end;
            continue;
        }
        ++index;
    }

    return EncodeState(JavaScanMode::None);
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    enum class ExpectedName {
        None,
        Type,
    };

    ExpectedName expected_name = ExpectedName::None;
    bool type_context = false;

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

        if (ch == '@') {
            const size_t end = ScanAnnotation(line, index);
            if (end > index + 1) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Macro);
                index = end;
                continue;
            }
        }

        if (ch == ';' || ch == '{' || ch == '}' || ch == '=') {
            type_context = false;
            expected_name = ExpectedName::None;
            ++index;
            continue;
        }

        if (ch == ',' || ch == '(' || ch == '<' || ch == '>' || ch == '[' || ch == ']' || ch == '?' || ch == '&') {
            if (ch == ',' || ch == '(' || ch == '<' || ch == '?' || ch == '&') {
                type_context = true;
            }
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
            if (token == "class" || token == "interface" || token == "enum" || token == "record") {
                expected_name = ExpectedName::Type;
                type_context = false;
            } else if (token == "extends" || token == "implements" || token == "new" || token == "throws" ||
                       token == "instanceof" || token == "permits") {
                expected_name = ExpectedName::None;
                type_context = true;
            } else if (ContainsWord(kModifierKeywords, token)) {
                expected_name = ExpectedName::None;
                type_context = true;
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

LanguageId JavaHighlighter::language() const { return LanguageId::Java; }

SyntaxLineState JavaHighlighter::HighlightLine(std::string_view line,
                                               SyntaxLineState in_state,
                                               std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    const SyntaxLineState next_state = AppendProtectedSpans(line, in_state, spans);
    AppendLexicalSpans(line, spans);
    return next_state;
}

}  // namespace patchwork
