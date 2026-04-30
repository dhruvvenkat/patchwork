#include "syntax/javascript_highlighter.h"

#include <array>
#include <cctype>
#include <optional>
#include <string_view>

namespace patchwork {

namespace {

enum class JavaScriptScanMode : uint64_t {
    None = 0,
    BlockComment = 1,
    SingleQuote = 2,
    DoubleQuote = 3,
    TemplateString = 4,
};

constexpr std::array<std::string_view, 39> kJavaScriptKeywords = {
    "async",    "await",    "break",   "case",     "catch",   "class",   "const",   "continue",
    "debugger", "default",  "delete",  "do",       "else",    "export",  "extends", "false",
    "finally",  "for",      "from",    "function", "if",      "import",  "in",      "instanceof",
    "let",      "new",      "null",    "of",       "return",  "static",  "super",   "switch",
    "this",     "throw",    "true",    "try",      "typeof",  "var",     "while",
};

constexpr std::array<std::string_view, 17> kTypeScriptKeywords = {
    "abstract", "as",        "asserts",   "declare", "enum",     "implements",
    "infer",    "interface", "is",        "keyof",   "module",   "namespace",
    "override", "private",   "protected", "public",  "readonly",
};

constexpr std::array<std::string_view, 23> kSharedTypeKeywords = {
    "Array",   "bigint", "BigInt",   "boolean", "Boolean", "Date",      "Error",   "Map",
    "never",   "number", "Number",   "Object",  "Promise", "ReadonlyArray",
    "RegExp",  "Set",    "string",   "String",  "symbol",  "Symbol",    "undefined",
    "unknown", "void",
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

SyntaxLineState EncodeState(JavaScriptScanMode mode) {
    return {.value = static_cast<uint64_t>(mode)};
}

JavaScriptScanMode DecodeMode(SyntaxLineState state) {
    return static_cast<JavaScriptScanMode>(state.value);
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

bool IsKeyword(std::string_view token, bool is_typescript) {
    return ContainsWord(kJavaScriptKeywords, token) || (is_typescript && ContainsWord(kTypeScriptKeywords, token));
}

bool IsTypeKeyword(std::string_view token, bool is_typescript) {
    return ContainsWord(kSharedTypeKeywords, token) ||
           (is_typescript && (token == "any" || token == "object"));
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

std::optional<size_t> FindTemplateStringEnd(std::string_view line, size_t search_index) {
    size_t index = search_index;
    while (index < line.size()) {
        if (line[index] == '\\' && index + 1 < line.size()) {
            index += 2;
            continue;
        }
        if (line[index] == '`') {
            return index + 1;
        }
        ++index;
    }
    return std::nullopt;
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

size_t ScanNumberLiteral(std::string_view line, size_t index) {
    if (index + 1 < line.size() && line[index] == '0') {
        const char prefix = line[index + 1];
        if (prefix == 'x' || prefix == 'X') {
            index = ScanDigitsWithUnderscores(line, index + 2, 16);
        } else if (prefix == 'o' || prefix == 'O') {
            index = ScanDigitsWithUnderscores(line, index + 2, 8);
        } else if (prefix == 'b' || prefix == 'B') {
            index = ScanDigitsWithUnderscores(line, index + 2, 2);
        } else {
            index = ScanDigitsWithUnderscores(line, index, 10);
        }
    } else {
        index = ScanDigitsWithUnderscores(line, index, 10);
    }

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

    if (index < line.size() && line[index] == 'n') {
        ++index;
    }
    return index;
}

bool IsFunctionCall(std::string_view line, size_t identifier_end) {
    const size_t next = SkipWhitespace(line, identifier_end);
    return next < line.size() && line[next] == '(';
}

bool LooksLikeArrowFunctionAssignment(std::string_view line, size_t identifier_end) {
    size_t index = SkipWhitespace(line, identifier_end);
    bool saw_equals = false;
    while (index < line.size()) {
        if (line[index] == ';' || line[index] == '{') {
            return false;
        }
        if (!saw_equals && line[index] == '=') {
            if (index + 1 < line.size() && line[index + 1] == '>') {
                return true;
            }
            if (index + 1 < line.size() && line[index + 1] == '=') {
                return false;
            }
            saw_equals = true;
            ++index;
            continue;
        }
        if (saw_equals && index + 1 < line.size() && line[index] == '=' && line[index + 1] == '>') {
            return true;
        }
        ++index;
    }
    return false;
}

size_t ScanDecorator(std::string_view line, size_t index) {
    size_t cursor = index + 1;
    while (cursor < line.size() && IsIdentifierBody(line[cursor])) {
        ++cursor;
    }
    return cursor;
}

SyntaxLineState AppendProtectedSpans(std::string_view line,
                                     SyntaxLineState in_state,
                                     std::vector<SyntaxSpan>* spans) {
    size_t index = 0;
    const JavaScriptScanMode active_mode = DecodeMode(in_state);
    if (active_mode != JavaScriptScanMode::None) {
        std::optional<size_t> end;
        switch (active_mode) {
            case JavaScriptScanMode::BlockComment:
                end = FindBlockCommentEnd(line, 0);
                break;
            case JavaScriptScanMode::SingleQuote:
                end = FindEscapedQuoteEnd(line, 0, '\'');
                break;
            case JavaScriptScanMode::DoubleQuote:
                end = FindEscapedQuoteEnd(line, 0, '"');
                break;
            case JavaScriptScanMode::TemplateString:
                end = FindTemplateStringEnd(line, 0);
                break;
            case JavaScriptScanMode::None:
                break;
        }

        const size_t span_end = end.has_value() ? *end : line.size();
        AppendSpan(spans, 0, span_end,
                   active_mode == JavaScriptScanMode::BlockComment ? SyntaxTokenKind::Comment : SyntaxTokenKind::String);
        if (!end.has_value()) {
            return EncodeState(active_mode);
        }
        index = span_end;
    }

    while (index < line.size()) {
        if (line.compare(index, 2, "//") == 0) {
            AppendSpan(spans, index, line.size(), SyntaxTokenKind::Comment);
            return EncodeState(JavaScriptScanMode::None);
        }
        if (line.compare(index, 2, "/*") == 0) {
            const std::optional<size_t> end = FindBlockCommentEnd(line, index + 2);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::Comment);
            if (!end.has_value()) {
                return EncodeState(JavaScriptScanMode::BlockComment);
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
                return EncodeState(quote == '\'' ? JavaScriptScanMode::SingleQuote : JavaScriptScanMode::DoubleQuote);
            }
            index = span_end;
            continue;
        }
        if (line[index] == '`') {
            const std::optional<size_t> end = FindTemplateStringEnd(line, index + 1);
            const size_t span_end = end.has_value() ? *end : line.size();
            AppendSpan(spans, index, span_end, SyntaxTokenKind::String);
            if (!end.has_value()) {
                return EncodeState(JavaScriptScanMode::TemplateString);
            }
            index = span_end;
            continue;
        }
        ++index;
    }

    return EncodeState(JavaScriptScanMode::None);
}

void AppendLexicalSpans(std::string_view line, std::vector<SyntaxSpan>* spans, LanguageId language_id) {
    const bool is_typescript = language_id == LanguageId::TypeScript;
    enum class ExpectedName {
        None,
        Function,
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

        if (is_typescript && ch == ':') {
            type_context = true;
            ++index;
            continue;
        }

        if (ch == '=' || ch == ';' || ch == '{' || ch == '}' || ch == ')' || ch == ',') {
            type_context = false;
            if (ch != '=') {
                expected_name = ExpectedName::None;
            }
            ++index;
            continue;
        }

        if (ch == '@') {
            const size_t end = ScanDecorator(line, index);
            if (end > index + 1) {
                AppendSpan(spans, index, end, SyntaxTokenKind::Macro);
                index = end;
                continue;
            }
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
        if (IsKeyword(token, is_typescript)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Keyword);
            if (token == "function") {
                expected_name = ExpectedName::Function;
            } else if (token == "class" || token == "interface" || token == "enum" || token == "type") {
                expected_name = ExpectedName::Type;
                type_context = false;
            } else if (token == "extends" || token == "implements" || token == "new") {
                expected_name = ExpectedName::Type;
                type_context = true;
            } else if (token == "as" || token == "is" || token == "keyof" || token == "readonly") {
                type_context = true;
                expected_name = ExpectedName::None;
            } else {
                expected_name = ExpectedName::None;
            }
            index = end;
            continue;
        }

        if (expected_name == ExpectedName::Function) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Function);
            expected_name = ExpectedName::None;
            index = end;
            continue;
        }

        if (expected_name == ExpectedName::Type) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            expected_name = ExpectedName::None;
            index = end;
            continue;
        }

        if (IsTypeKeyword(token, is_typescript) || (type_context && LooksLikeTypeName(token))) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Type);
            index = end;
            continue;
        }

        if (LooksLikeArrowFunctionAssignment(line, end) || IsFunctionCall(line, end)) {
            AppendSpan(spans, index, end, SyntaxTokenKind::Function);
            index = end;
            continue;
        }

        index = end;
    }
}

}  // namespace

JavaScriptHighlighter::JavaScriptHighlighter(LanguageId language_id) : language_id_(language_id) {}

LanguageId JavaScriptHighlighter::language() const { return language_id_; }

SyntaxLineState JavaScriptHighlighter::HighlightLine(std::string_view line,
                                                     SyntaxLineState in_state,
                                                     std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    const SyntaxLineState next_state = AppendProtectedSpans(line, in_state, spans);
    AppendLexicalSpans(line, spans, language_id_);
    return next_state;
}

}  // namespace patchwork
