#include "syntax/markdown_highlighter.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

#include "syntax/registry.h"

namespace flowstate {

namespace {

enum class MarkdownMode : uint64_t {
    None = 0,
    FencedCode = 1,
};

enum class FenceMarkerKind : uint64_t {
    None = 0,
    Backtick = 1,
    Tilde = 2,
};

constexpr uint64_t kModeMask = 0xFULL;
constexpr int kMarkerShift = 4;
constexpr uint64_t kMarkerMask = 0x3ULL << kMarkerShift;
constexpr int kFenceCountShift = 6;
constexpr uint64_t kFenceCountMask = 0xFFULL << kFenceCountShift;
constexpr int kLanguageShift = 14;
constexpr uint64_t kLanguageMask = 0xFULL << kLanguageShift;
constexpr int kInnerStateShift = 18;

struct ParsedFence {
    size_t indent = 0;
    FenceMarkerKind marker = FenceMarkerKind::None;
    uint8_t count = 0;
    std::string_view raw_tail;
    std::string_view info;
};

template <typename Predicate>
size_t SkipWhile(std::string_view text, size_t index, Predicate predicate) {
    while (index < text.size() && predicate(text[index])) {
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

std::string_view TrimWhitespace(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string ToLower(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

LanguageId DetectFenceLanguage(std::string_view info) {
    const std::string_view trimmed = TrimWhitespace(info);
    const size_t token_end = trimmed.find_first_of(" \t");
    const std::string token = ToLower(trimmed.substr(0, token_end));

    if (token == "c") {
        return LanguageId::C;
    }
    if (token == "cc" || token == "cpp" || token == "cxx" || token == "c++" || token == "hpp" || token == "h") {
        return LanguageId::Cpp;
    }
    if (token == "rust" || token == "rs") {
        return LanguageId::Rust;
    }
    if (token == "python" || token == "py") {
        return LanguageId::Python;
    }
    if (token == "javascript" || token == "js") {
        return LanguageId::JavaScript;
    }
    if (token == "typescript" || token == "ts" || token == "tsx") {
        return LanguageId::TypeScript;
    }
    if (token == "java") {
        return LanguageId::Java;
    }
    if (token == "go" || token == "golang") {
        return LanguageId::Go;
    }
    return LanguageId::PlainText;
}

SyntaxLineState EncodeState(MarkdownMode mode,
                            FenceMarkerKind marker = FenceMarkerKind::None,
                            uint8_t fence_count = 0,
                            LanguageId language_id = LanguageId::PlainText,
                            uint64_t inner_state = 0) {
    return {.value = (inner_state << kInnerStateShift) | (static_cast<uint64_t>(language_id) << kLanguageShift) |
                     (static_cast<uint64_t>(fence_count) << kFenceCountShift) |
                     (static_cast<uint64_t>(marker) << kMarkerShift) | static_cast<uint64_t>(mode)};
}

MarkdownMode DecodeMode(SyntaxLineState state) {
    return static_cast<MarkdownMode>(state.value & kModeMask);
}

FenceMarkerKind DecodeFenceMarker(SyntaxLineState state) {
    return static_cast<FenceMarkerKind>((state.value & kMarkerMask) >> kMarkerShift);
}

uint8_t DecodeFenceCount(SyntaxLineState state) {
    return static_cast<uint8_t>((state.value & kFenceCountMask) >> kFenceCountShift);
}

LanguageId DecodeLanguageId(SyntaxLineState state) {
    return static_cast<LanguageId>((state.value & kLanguageMask) >> kLanguageShift);
}

SyntaxLineState DecodeInnerState(SyntaxLineState state) {
    return {.value = state.value >> kInnerStateShift};
}

FenceMarkerKind MarkerKindForChar(char ch) {
    if (ch == '`') {
        return FenceMarkerKind::Backtick;
    }
    if (ch == '~') {
        return FenceMarkerKind::Tilde;
    }
    return FenceMarkerKind::None;
}

char MarkerChar(FenceMarkerKind marker) {
    switch (marker) {
        case FenceMarkerKind::Backtick:
            return '`';
        case FenceMarkerKind::Tilde:
            return '~';
        case FenceMarkerKind::None:
            return '\0';
    }
    return '\0';
}

std::optional<ParsedFence> ParseFence(std::string_view line) {
    const size_t indent = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
    if (indent >= line.size()) {
        return std::nullopt;
    }

    const FenceMarkerKind marker = MarkerKindForChar(line[indent]);
    if (marker == FenceMarkerKind::None) {
        return std::nullopt;
    }

    size_t cursor = indent;
    while (cursor < line.size() && line[cursor] == line[indent]) {
        ++cursor;
    }

    const size_t count = cursor - indent;
    if (count < 3) {
        return std::nullopt;
    }

    const std::string_view raw_tail = line.substr(cursor);
    return ParsedFence{
        .indent = indent,
        .marker = marker,
        .count = static_cast<uint8_t>(std::min<size_t>(count, 255)),
        .raw_tail = raw_tail,
        .info = TrimWhitespace(raw_tail),
    };
}

bool IsClosingFence(std::string_view line, FenceMarkerKind marker, uint8_t fence_count) {
    const std::optional<ParsedFence> parsed = ParseFence(line);
    if (!parsed.has_value() || parsed->marker != marker || parsed->count < fence_count) {
        return false;
    }
    return TrimWhitespace(parsed->raw_tail).empty();
}

bool IsHeadingLine(std::string_view line, size_t* start) {
    if (start == nullptr) {
        return false;
    }

    size_t index = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
    size_t hash_count = 0;
    while (index + hash_count < line.size() && line[index + hash_count] == '#') {
        ++hash_count;
    }
    if (hash_count == 0 || hash_count > 6) {
        return false;
    }

    const size_t next = index + hash_count;
    if (next < line.size() && !std::isspace(static_cast<unsigned char>(line[next]))) {
        return false;
    }

    *start = index;
    return true;
}

bool IsQuoteLine(std::string_view line, size_t* start) {
    if (start == nullptr) {
        return false;
    }
    const size_t index = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
    if (index >= line.size() || line[index] != '>') {
        return false;
    }
    *start = index;
    return true;
}

bool IsHorizontalRule(std::string_view line, size_t* start) {
    if (start == nullptr) {
        return false;
    }

    const size_t indent = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
    const std::string_view trimmed = TrimWhitespace(line.substr(indent));
    if (trimmed.size() < 3) {
        return false;
    }

    const char marker = trimmed.front();
    if (marker != '-' && marker != '*' && marker != '_') {
        return false;
    }

    size_t marker_count = 0;
    for (char ch : trimmed) {
        if (ch == marker) {
            ++marker_count;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            return false;
        }
    }

    if (marker_count < 3) {
        return false;
    }

    *start = indent;
    return true;
}

bool ParseListMarker(std::string_view line, size_t* marker_start, size_t* marker_end) {
    if (marker_start == nullptr || marker_end == nullptr) {
        return false;
    }

    const size_t start = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
    if (start >= line.size()) {
        return false;
    }

    size_t end = start;
    if (line[start] == '-' || line[start] == '*' || line[start] == '+') {
        end = start + 1;
        if (end >= line.size() || !std::isspace(static_cast<unsigned char>(line[end]))) {
            return false;
        }
        while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        if (end + 2 < line.size() && line[end] == '[' && (line[end + 1] == ' ' || line[end + 1] == 'x' || line[end + 1] == 'X') &&
            line[end + 2] == ']') {
            end += 3;
            if (end < line.size() && std::isspace(static_cast<unsigned char>(line[end]))) {
                ++end;
            }
        }
        *marker_start = start;
        *marker_end = end;
        return true;
    }

    if (!std::isdigit(static_cast<unsigned char>(line[start]))) {
        return false;
    }

    end = start;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
        ++end;
    }
    if (end >= line.size() || line[end] != '.') {
        return false;
    }
    ++end;
    if (end >= line.size() || !std::isspace(static_cast<unsigned char>(line[end]))) {
        return false;
    }
    while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end]))) {
        ++end;
    }
    *marker_start = start;
    *marker_end = end;
    return true;
}

void AppendInlineCodeSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    for (size_t index = 0; index < line.size();) {
        size_t covered_end = 0;
        if (spans != nullptr && SpanEndAt(*spans, index, &covered_end)) {
            index = covered_end;
            continue;
        }

        if (line[index] != '`') {
            ++index;
            continue;
        }

        size_t tick_count = 1;
        while (index + tick_count < line.size() && line[index + tick_count] == '`') {
            ++tick_count;
        }

        size_t search = index + tick_count;
        bool matched = false;
        while (search + tick_count <= line.size()) {
            search = line.find('`', search);
            if (search == std::string_view::npos || search + tick_count > line.size()) {
                break;
            }

            bool same_run = true;
            for (size_t offset = 0; offset < tick_count; ++offset) {
                if (line[search + offset] != '`') {
                    same_run = false;
                    break;
                }
            }
            if (!same_run) {
                ++search;
                continue;
            }

            AppendSpan(spans, index, search + tick_count, SyntaxTokenKind::InlineCode);
            index = search + tick_count;
            matched = true;
            break;
        }

        if (!matched) {
            index += tick_count;
        }
    }
}

void AppendLinkSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    for (size_t index = 0; index < line.size();) {
        size_t covered_end = 0;
        if (spans != nullptr && SpanEndAt(*spans, index, &covered_end)) {
            index = covered_end;
            continue;
        }

        size_t text_start = index;
        if (line[index] == '!' && index + 1 < line.size() && line[index + 1] == '[') {
            text_start = index;
            ++index;
        }
        if (line[index] != '[') {
            ++index;
            continue;
        }

        const size_t close_bracket = line.find(']', index + 1);
        if (close_bracket == std::string_view::npos || close_bracket + 1 >= line.size() || line[close_bracket + 1] != '(') {
            index = index + 1;
            continue;
        }

        const size_t close_paren = line.find(')', close_bracket + 2);
        if (close_paren == std::string_view::npos) {
            index = index + 1;
            continue;
        }

        AppendSpan(spans, text_start, close_bracket + 1, SyntaxTokenKind::LinkText);
        AppendSpan(spans, close_bracket + 1, close_paren + 1, SyntaxTokenKind::LinkUrl);
        index = close_paren + 1;
    }
}

void AppendEmphasisSpans(std::string_view line, std::vector<SyntaxSpan>* spans) {
    for (size_t index = 0; index < line.size();) {
        size_t covered_end = 0;
        if (spans != nullptr && SpanEndAt(*spans, index, &covered_end)) {
            index = covered_end;
            continue;
        }

        if (line[index] != '*' && line[index] != '_') {
            ++index;
            continue;
        }

        const char marker = line[index];
        size_t run_length = 1;
        if (index + 1 < line.size() && line[index + 1] == marker) {
            run_length = 2;
        }

        size_t search = index + run_length;
        bool matched = false;
        while (search + run_length <= line.size()) {
            search = line.find(marker, search);
            if (search == std::string_view::npos || search + run_length > line.size()) {
                break;
            }

            bool same_run = true;
            for (size_t offset = 0; offset < run_length; ++offset) {
                if (line[search + offset] != marker) {
                    same_run = false;
                    break;
                }
            }
            if (!same_run) {
                ++search;
                continue;
            }

            AppendSpan(spans, index, search + run_length, SyntaxTokenKind::Emphasis);
            index = search + run_length;
            matched = true;
            break;
        }

        if (!matched) {
            index += run_length;
        }
    }
}

}  // namespace

LanguageId MarkdownHighlighter::language() const { return LanguageId::Markdown; }

SyntaxLineState MarkdownHighlighter::HighlightLine(std::string_view line,
                                                   SyntaxLineState in_state,
                                                   std::vector<SyntaxSpan>* spans) const {
    if (spans != nullptr) {
        spans->clear();
    }

    if (DecodeMode(in_state) == MarkdownMode::FencedCode) {
        const FenceMarkerKind fence_marker = DecodeFenceMarker(in_state);
        const uint8_t fence_count = DecodeFenceCount(in_state);
        if (IsClosingFence(line, fence_marker, fence_count)) {
            const size_t start = SkipWhile(line, 0, [](char ch) { return ch == ' '; });
            AppendSpan(spans, start, line.size(), SyntaxTokenKind::CodeFence);
            return {};
        }

        const LanguageId language_id = DecodeLanguageId(in_state);
        const ISyntaxHighlighter& inner_highlighter = HighlighterForLanguage(language_id);
        const SyntaxLineState next_inner_state = inner_highlighter.HighlightLine(line, DecodeInnerState(in_state), spans);
        return EncodeState(MarkdownMode::FencedCode, fence_marker, fence_count, language_id, next_inner_state.value);
    }

    if (const std::optional<ParsedFence> fence = ParseFence(line); fence.has_value()) {
        AppendSpan(spans, fence->indent, line.size(), SyntaxTokenKind::CodeFence);
        const LanguageId language_id = DetectFenceLanguage(fence->info);
        const ISyntaxHighlighter& inner_highlighter = HighlighterForLanguage(language_id);
        return EncodeState(MarkdownMode::FencedCode,
                           fence->marker,
                           fence->count,
                           language_id,
                           inner_highlighter.InitialState().value);
    }

    size_t block_start = 0;
    if (IsHeadingLine(line, &block_start)) {
        AppendSpan(spans, block_start, line.size(), SyntaxTokenKind::Heading);
        return {};
    }
    if (IsQuoteLine(line, &block_start)) {
        AppendSpan(spans, block_start, line.size(), SyntaxTokenKind::Quote);
        return {};
    }
    if (IsHorizontalRule(line, &block_start)) {
        AppendSpan(spans, block_start, line.size(), SyntaxTokenKind::ListMarker);
        return {};
    }

    size_t list_start = 0;
    size_t list_end = 0;
    if (ParseListMarker(line, &list_start, &list_end)) {
        AppendSpan(spans, list_start, list_end, SyntaxTokenKind::ListMarker);
    }

    AppendInlineCodeSpans(line, spans);
    AppendLinkSpans(line, spans);
    AppendEmphasisSpans(line, spans);
    return {};
}

}  // namespace flowstate
