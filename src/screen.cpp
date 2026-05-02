#include "screen.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>

#include "git_status.h"
#include "selection.h"
#include "syntax/registry.h"
#include "syntax/theme.h"

namespace patchwork {

namespace {

constexpr std::string_view kGutterSeparator = "\xE2\x94\x82";
constexpr std::string_view kGitDashedBar = "\xE2\x94\x86";
constexpr std::string_view kGitDeletedTriangle = "\xE2\x96\xB8";
constexpr std::string_view kGitAddedColor = "\x1b[38;5;71m";
constexpr std::string_view kGitModifiedColor = "\x1b[38;5;39m";
constexpr std::string_view kGitDeletedColor = "\x1b[38;5;196m";
constexpr std::string_view kResetForeground = "\x1b[39m";
constexpr auto kGitStatusRefreshInterval = std::chrono::milliseconds(750);

enum class AiDiffPhase {
    Outside,
    AwaitNewFileHeader,
    AwaitHunkHeader,
    InHunk,
};

enum class AiDiffLineKind {
    Other,
    FileHeaderOld,
    FileHeaderNew,
    HunkHeader,
    Context,
    Add,
    Remove,
    Meta,
};

struct AiDiffRenderState {
    AiDiffPhase phase = AiDiffPhase::Outside;
    SyntaxLineState old_syntax_state;
    SyntaxLineState new_syntax_state;
};

struct FileRenderState {
    SyntaxLineState syntax_state;
    std::vector<char> delimiter_stack;
};

std::string EscapeLine(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '\t') {
            output.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            output.push_back('?');
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

std::string DecoratePatchLine(const std::string& line) {
    if (!line.empty() && line[0] == '+') {
        return "\x1b[32m" + line + "\x1b[39m";
    }
    if (!line.empty() && line[0] == '-') {
        return "\x1b[31m" + line + "\x1b[39m";
    }
    if (line.rfind("@@", 0) == 0) {
        return "\x1b[36m" + line + "\x1b[39m";
    }
    return line;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string RenderVisibleText(std::string_view line, size_t col_offset, size_t cols) {
    if (cols == 0 || col_offset >= line.size()) {
        return {};
    }
    return std::string(line.substr(col_offset, cols));
}

size_t LineNumberDigits(const Buffer& buffer) {
    return std::max<size_t>(1, std::to_string(buffer.lineCount()).size());
}

size_t GutterWidth(const Buffer& buffer) {
    return LineNumberDigits(buffer) + 2;
}

bool ShowsLineNumbers(const EditorState& state) {
    return state.activeView() == ViewKind::File;
}

std::string GitMarkerForRow(size_t row, const GitLineStatus& git_status) {
    if (!git_status.available || row >= git_status.lines.size()) {
        return std::string(kGutterSeparator);
    }

    switch (git_status.lines[row].marker) {
        case GitLineMarker::Added:
            return std::string(kGitAddedColor) + std::string(kGitDashedBar) + std::string(kResetForeground);
        case GitLineMarker::Modified:
            return std::string(kGitModifiedColor) + std::string(kGitDashedBar) + std::string(kResetForeground);
        case GitLineMarker::Deleted:
            return std::string(kGitDeletedColor) + std::string(kGitDeletedTriangle) + std::string(kResetForeground);
        case GitLineMarker::Clean:
            return std::string(kGutterSeparator);
    }
    return std::string(kGutterSeparator);
}

const std::vector<std::string>& ExpandedPreviousLinesForRow(const EditorState& state,
                                                           const GitLineStatus& git_status,
                                                           size_t row) {
    static const std::vector<std::string> kEmptyPreviousLines;
    if (!state.isGitChangePeekExpanded(row) || !git_status.available || row >= git_status.lines.size()) {
        return kEmptyPreviousLines;
    }
    return git_status.lines[row].previous_lines;
}

size_t ExpandedGitRowsBefore(const EditorState& state,
                             const GitLineStatus& git_status,
                             size_t start_row,
                             size_t end_row) {
    size_t count = 0;
    for (size_t row = start_row; row < end_row && row < git_status.lines.size(); ++row) {
        count += ExpandedPreviousLinesForRow(state, git_status, row).size();
    }
    return count;
}

struct GitStatusCacheEntry {
    std::filesystem::path file_path;
    size_t line_count = 0;
    std::chrono::steady_clock::time_point refreshed_at;
    GitLineStatus status;
};

const GitLineStatus& GitStatusForBuffer(const Buffer& buffer) {
    static GitStatusCacheEntry cache;

    const std::filesystem::path file_path = buffer.path().value_or(std::filesystem::path());
    const auto now = std::chrono::steady_clock::now();
    if (cache.file_path == file_path && cache.line_count == buffer.lineCount() &&
        now - cache.refreshed_at < kGitStatusRefreshInterval) {
        return cache.status;
    }

    cache.file_path = file_path;
    cache.line_count = buffer.lineCount();
    cache.refreshed_at = now;
    cache.status = LoadGitLineStatus(file_path, buffer.lineCount());
    return cache.status;
}

std::string RenderLineNumber(size_t row, const Buffer& buffer, const GitLineStatus& git_status) {
    std::ostringstream output;
    output << std::setw(static_cast<int>(LineNumberDigits(buffer))) << (row + 1)
           << GitMarkerForRow(row, git_status) << ' ';
    return output.str();
}

std::string RenderGitPreviousLine(const Buffer& buffer,
                                  std::string_view previous_line,
                                  size_t col_offset,
                                  size_t cols) {
    std::ostringstream output;
    output << std::string(LineNumberDigits(buffer), ' ') << std::string(kGitDeletedColor)
           << kGitDeletedTriangle << kResetForeground << ' ';
    output << std::string(kGitDeletedColor);
    if (col_offset == 0 && cols > 0) {
        output << "- ";
        if (cols > 2) {
            output << RenderVisibleText(previous_line, 0, cols - 2);
        }
    } else if (col_offset > 0) {
        output << RenderVisibleText(previous_line, col_offset, cols);
    }
    output << kResetForeground;
    return output.str();
}

AiDiffLineKind ClassifyAiDiffLine(std::string_view line, AiDiffPhase phase) {
    if (StartsWith(line, "@@")) {
        return AiDiffLineKind::HunkHeader;
    }
    if (StartsWith(line, "--- ")) {
        return AiDiffLineKind::FileHeaderOld;
    }
    if (StartsWith(line, "+++ ")) {
        return AiDiffLineKind::FileHeaderNew;
    }
    if (phase != AiDiffPhase::InHunk || line.empty()) {
        return AiDiffLineKind::Other;
    }

    switch (line.front()) {
        case ' ':
            return AiDiffLineKind::Context;
        case '+':
            return AiDiffLineKind::Add;
        case '-':
            return AiDiffLineKind::Remove;
        case '\\':
            return AiDiffLineKind::Meta;
        default:
            return AiDiffLineKind::Other;
    }
}

SyntaxTokenKind TokenKindAt(const std::vector<SyntaxSpan>& spans, size_t index) {
    for (auto span = spans.rbegin(); span != spans.rend(); ++span) {
        if (index >= span->start && index < span->end) {
            return span->kind;
        }
    }
    return SyntaxTokenKind::Default;
}

std::string_view BraceColorCodeForDepth(int depth) {
    switch (depth % 3) {
        case 0:
            return "\x1b[38;5;211m";
        case 1:
            return "\x1b[38;5;75m";
        case 2:
            return "\x1b[38;5;78m";
    }
    return "\x1b[38;5;211m";
}

bool IsOpeningDelimiter(char ch) {
    return ch == '{' || ch == '[' || ch == '(';
}

bool IsClosingDelimiter(char ch) {
    return ch == '}' || ch == ']' || ch == ')';
}

char MatchingDelimiter(char ch) {
    switch (ch) {
        case '{':
            return '}';
        case '}':
            return '{';
        case '[':
            return ']';
        case ']':
            return '[';
        case '(':
            return ')';
        case ')':
            return '(';
    }
    return '\0';
}

bool IsRainbowBrace(char ch, SyntaxTokenKind token_kind) {
    return token_kind == SyntaxTokenKind::Default && (IsOpeningDelimiter(ch) || IsClosingDelimiter(ch));
}

bool DelimitersMatch(char opening, char closing) {
    return IsOpeningDelimiter(opening) && MatchingDelimiter(opening) == closing;
}

bool IsBracePosition(const Cursor& cursor, size_t row, size_t col) {
    return cursor.row == row && cursor.col == col;
}

std::vector<SyntaxLineState> BuildLineStates(const Buffer& buffer, const ISyntaxHighlighter& highlighter) {
    std::vector<SyntaxLineState> states(buffer.lineCount() + 1, highlighter.InitialState());
    for (size_t row = 0; row < buffer.lineCount(); ++row) {
        states[row + 1] = highlighter.HighlightLine(buffer.line(row), states[row], nullptr);
    }
    return states;
}

std::optional<Cursor> FindMatchingBrace(const Buffer& buffer,
                                        const ISyntaxHighlighter& highlighter,
                                        const Cursor& cursor) {
    if (cursor.row >= buffer.lineCount()) {
        return std::nullopt;
    }

    const std::string& cursor_line = buffer.line(cursor.row);
    if (cursor.col >= cursor_line.size()) {
        return std::nullopt;
    }

    const std::vector<SyntaxLineState> line_states = BuildLineStates(buffer, highlighter);
    std::vector<SyntaxSpan> spans;
    highlighter.HighlightLine(cursor_line, line_states[cursor.row], &spans);
    const SyntaxTokenKind cursor_token_kind = TokenKindAt(spans, cursor.col);
    if (!IsRainbowBrace(cursor_line[cursor.col], cursor_token_kind)) {
        return std::nullopt;
    }

    const char cursor_delimiter = cursor_line[cursor.col];
    if (IsOpeningDelimiter(cursor_delimiter)) {
        const char matching_delimiter = MatchingDelimiter(cursor_delimiter);
        int depth = 0;
        for (size_t row = cursor.row; row < buffer.lineCount(); ++row) {
            spans.clear();
            const std::string& line = buffer.line(row);
            highlighter.HighlightLine(line, line_states[row], &spans);
            const size_t start = row == cursor.row ? cursor.col : 0;
            for (size_t col = start; col < line.size(); ++col) {
                const SyntaxTokenKind token_kind = TokenKindAt(spans, col);
                if (!IsRainbowBrace(line[col], token_kind)) {
                    continue;
                }
                if (line[col] == cursor_delimiter) {
                    ++depth;
                } else if (line[col] == matching_delimiter) {
                    --depth;
                    if (depth == 0) {
                        return Cursor{row, col};
                    }
                }
            }
        }
        return std::nullopt;
    }

    const char matching_delimiter = MatchingDelimiter(cursor_delimiter);
    int depth = 0;
    for (size_t row = cursor.row + 1; row-- > 0;) {
        spans.clear();
        const std::string& line = buffer.line(row);
        highlighter.HighlightLine(line, line_states[row], &spans);
        const size_t start = row == cursor.row ? cursor.col + 1 : line.size();
        for (size_t col = start; col-- > 0;) {
            const SyntaxTokenKind token_kind = TokenKindAt(spans, col);
            if (!IsRainbowBrace(line[col], token_kind)) {
                continue;
            }
            if (line[col] == cursor_delimiter) {
                ++depth;
            } else if (line[col] == matching_delimiter) {
                --depth;
                if (depth == 0) {
                    return Cursor{row, col};
                }
            }
        }
    }
    return std::nullopt;
}

std::vector<char> AdvanceDelimiterStack(std::string_view line,
                                        const std::vector<SyntaxSpan>& highlights,
                                        std::vector<char> delimiter_stack) {
    for (size_t index = 0; index < line.size(); ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        if (!IsRainbowBrace(line[index], token_kind)) {
            continue;
        }

        if (IsOpeningDelimiter(line[index])) {
            delimiter_stack.push_back(line[index]);
        } else if (!delimiter_stack.empty()) {
            delimiter_stack.pop_back();
        }
    }
    return delimiter_stack;
}

std::string RenderHighlightedCode(std::string_view code,
                                  size_t col_offset,
                                  size_t cols,
                                  const ISyntaxHighlighter& highlighter,
                                  SyntaxLineState line_state,
                                  SyntaxLineState* next_line_state) {
    std::string rendered;
    rendered.reserve(cols + 16);

    std::vector<SyntaxSpan> highlights;
    const SyntaxLineState updated_state = highlighter.HighlightLine(code, line_state, &highlights);
    if (next_line_state != nullptr) {
        *next_line_state = updated_state;
    }

    if (col_offset >= code.size()) {
        return rendered;
    }

    const size_t end = std::min(code.size(), col_offset + cols);
    SyntaxTokenKind active_token_kind = SyntaxTokenKind::Default;
    for (size_t index = col_offset; index < end; ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        if (token_kind != active_token_kind) {
            rendered += ColorCodeForToken(token_kind);
            active_token_kind = token_kind;
        }

        const char ch = code[index];
        if (ch == '\t') {
            rendered.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            rendered.push_back('?');
        } else {
            rendered.push_back(ch);
        }
    }

    if (active_token_kind != SyntaxTokenKind::Default) {
        rendered += std::string(ResetColorCode());
    }
    return rendered;
}

std::string DiffPrefixColor(AiDiffLineKind kind) {
    switch (kind) {
        case AiDiffLineKind::Add:
            return "\x1b[32m";
        case AiDiffLineKind::Remove:
            return "\x1b[31m";
        default:
            return {};
    }
}

AiDiffRenderState AdvanceAiDiffRenderState(std::string_view line,
                                           const ISyntaxHighlighter& highlighter,
                                           AiDiffRenderState state) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state.phase);

    switch (kind) {
        case AiDiffLineKind::FileHeaderOld:
            state.phase = AiDiffPhase::AwaitNewFileHeader;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::FileHeaderNew:
            state.phase = AiDiffPhase::AwaitHunkHeader;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::HunkHeader:
            state.phase = AiDiffPhase::InHunk;
            state.old_syntax_state = highlighter.InitialState();
            state.new_syntax_state = highlighter.InitialState();
            return state;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state.new_syntax_state;
            next_state = highlighter.HighlightLine(line.substr(1), next_state, nullptr);
            state.old_syntax_state = next_state;
            state.new_syntax_state = next_state;
            return state;
        }
        case AiDiffLineKind::Add:
            state.new_syntax_state = highlighter.HighlightLine(line.substr(1), state.new_syntax_state, nullptr);
            return state;
        case AiDiffLineKind::Remove:
            state.old_syntax_state = highlighter.HighlightLine(line.substr(1), state.old_syntax_state, nullptr);
            return state;
        case AiDiffLineKind::Meta:
            return state;
        case AiDiffLineKind::Other:
            if (state.phase == AiDiffPhase::InHunk && !line.empty()) {
                state.phase = AiDiffPhase::Outside;
                state.old_syntax_state = highlighter.InitialState();
                state.new_syntax_state = highlighter.InitialState();
            }
            return state;
    }

    return state;
}

AiDiffRenderState StateBeforeVisibleAiRow(const Buffer& buffer,
                                          const ISyntaxHighlighter& highlighter,
                                          size_t row) {
    AiDiffRenderState state;
    state.old_syntax_state = highlighter.InitialState();
    state.new_syntax_state = highlighter.InitialState();
    for (size_t index = 0; index < row && index < buffer.lineCount(); ++index) {
        state = AdvanceAiDiffRenderState(buffer.line(index), highlighter, state);
    }
    return state;
}

std::string RenderAiDiffCodeLine(std::string_view line,
                                 size_t col_offset,
                                 size_t cols,
                                 AiDiffLineKind kind,
                                 const ISyntaxHighlighter& highlighter,
                                 SyntaxLineState line_state,
                                 SyntaxLineState* next_line_state);

std::string RenderPatchPreviewLine(std::string_view line,
                                   size_t col_offset,
                                   size_t cols,
                                   const ISyntaxHighlighter& highlighter,
                                   const AiDiffRenderState& state_before,
                                   AiDiffRenderState* state_after) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state_before.phase);
    std::string rendered;

    switch (kind) {
        case AiDiffLineKind::HunkHeader:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderHighlightedCode(line.substr(1), col_offset > 0 ? col_offset - 1 : 0, cols, highlighter,
                                             state_before.new_syntax_state, &next_state);
            if (col_offset == 0) {
                rendered = " " + rendered;
            }
            break;
        }
        case AiDiffLineKind::Add: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state,
                                            &next_state);
            break;
        }
        case AiDiffLineKind::Remove:
        case AiDiffLineKind::Meta:
        case AiDiffLineKind::Other:
        case AiDiffLineKind::FileHeaderOld:
        case AiDiffLineKind::FileHeaderNew:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
    }

    if (state_after != nullptr) {
        *state_after = AdvanceAiDiffRenderState(line, highlighter, state_before);
    }
    return rendered;
}

std::string RenderAiDiffCodeLine(std::string_view line,
                                 size_t col_offset,
                                 size_t cols,
                                 AiDiffLineKind kind,
                                 const ISyntaxHighlighter& highlighter,
                                 SyntaxLineState line_state,
                                 SyntaxLineState* next_line_state) {
    if (line.empty() || cols == 0) {
        if (next_line_state != nullptr) {
            *next_line_state = line_state;
        }
        return {};
    }

    const bool show_prefix = col_offset == 0;
    const size_t code_col_offset = col_offset == 0 ? 0 : col_offset - 1;
    const size_t code_cols = cols > 0 ? cols - (show_prefix ? 1 : 0) : 0;

    std::string rendered;
    rendered.reserve(cols + 16);
    if (show_prefix) {
        const std::string prefix_color = DiffPrefixColor(kind);
        if (!prefix_color.empty()) {
            rendered += prefix_color;
        }
        rendered.push_back(line.front());
        if (!prefix_color.empty()) {
            rendered += "\x1b[39m";
        }
    }

    rendered += RenderHighlightedCode(line.substr(1), code_col_offset, code_cols, highlighter, line_state, next_line_state);
    return rendered;
}

std::string RenderAiScratchLine(std::string_view line,
                                size_t col_offset,
                                size_t cols,
                                const ISyntaxHighlighter& highlighter,
                                const AiDiffRenderState& state_before,
                                AiDiffRenderState* state_after) {
    const AiDiffLineKind kind = ClassifyAiDiffLine(line, state_before.phase);
    std::string rendered;

    switch (kind) {
        case AiDiffLineKind::HunkHeader:
        case AiDiffLineKind::FileHeaderOld:
        case AiDiffLineKind::FileHeaderNew:
            rendered = DecoratePatchLine(RenderVisibleText(line, col_offset, cols));
            break;
        case AiDiffLineKind::Context: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Add: {
            SyntaxLineState next_state = state_before.new_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.new_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Remove: {
            SyntaxLineState next_state = state_before.old_syntax_state;
            rendered = RenderAiDiffCodeLine(line, col_offset, cols, kind, highlighter, state_before.old_syntax_state, &next_state);
            break;
        }
        case AiDiffLineKind::Meta:
        case AiDiffLineKind::Other:
            rendered = RenderVisibleText(line, col_offset, cols);
            break;
    }

    if (state_after != nullptr) {
        *state_after = AdvanceAiDiffRenderState(line, highlighter, state_before);
    }
    return rendered;
}

FileRenderState StateBeforeVisibleRow(const Buffer& buffer,
                                      const ISyntaxHighlighter& highlighter,
                                      size_t row) {
    FileRenderState state{
        .syntax_state = highlighter.InitialState(),
    };
    std::vector<SyntaxSpan> scratch;
    for (size_t index = 0; index < row && index < buffer.lineCount(); ++index) {
        state.syntax_state = highlighter.HighlightLine(buffer.line(index), state.syntax_state, &scratch);
        state.delimiter_stack = AdvanceDelimiterStack(buffer.line(index), scratch, std::move(state.delimiter_stack));
    }
    return state;
}

std::string RenderFileLine(const EditorState& state,
                           const ISyntaxHighlighter& highlighter,
                           const std::string& line,
                           size_t row,
                           size_t col_offset,
                           size_t cols,
                           SyntaxLineState line_state,
                           std::vector<char> delimiter_stack,
                           const std::optional<Cursor>& active_brace,
                           const std::optional<Cursor>& matching_brace,
                           SyntaxLineState* next_line_state,
                           std::vector<char>* next_delimiter_stack) {
    std::string rendered;
    rendered.reserve(cols + 16);

    std::vector<SyntaxSpan> highlights;
    const SyntaxLineState updated_state = highlighter.HighlightLine(line, line_state, &highlights);
    if (next_line_state != nullptr) {
        *next_line_state = updated_state;
    }

    if (col_offset >= line.size()) {
        if (next_delimiter_stack != nullptr) {
            *next_delimiter_stack = AdvanceDelimiterStack(line, highlights, std::move(delimiter_stack));
        }
        return rendered;
    }

    const size_t end = std::min(line.size(), col_offset + cols);
    bool inverted = false;
    std::string_view active_color_code = ResetColorCode();
    bool active_bold = false;
    for (size_t index = 0; index < line.size(); ++index) {
        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        std::string_view desired_color_code = ColorCodeForToken(token_kind);
        if (IsRainbowBrace(line[index], token_kind)) {
            if (IsClosingDelimiter(line[index])) {
                const int color_depth =
                    delimiter_stack.empty() ? 0 : static_cast<int>(delimiter_stack.size()) - 1;
                desired_color_code = BraceColorCodeForDepth(color_depth);
                if (!delimiter_stack.empty()) {
                    delimiter_stack.pop_back();
                }
            } else {
                desired_color_code = BraceColorCodeForDepth(static_cast<int>(delimiter_stack.size()));
                delimiter_stack.push_back(line[index]);
            }
        }

        if (index < col_offset || index >= end) {
            continue;
        }

        const bool selected = IsPositionSelected(state.selection(), row, index);
        if (selected && !inverted) {
            rendered += "\x1b[7m";
            inverted = true;
        } else if (!selected && inverted) {
            rendered += "\x1b[27m";
            inverted = false;
        }

        const bool desired_bold =
            (active_brace.has_value() && IsBracePosition(*active_brace, row, index)) ||
            (matching_brace.has_value() && IsBracePosition(*matching_brace, row, index));
        if (desired_color_code != active_color_code) {
            rendered += desired_color_code;
            active_color_code = desired_color_code;
        }
        if (desired_bold && !active_bold) {
            rendered += "\x1b[1m";
            active_bold = true;
        } else if (!desired_bold && active_bold) {
            rendered += "\x1b[22m";
            active_bold = false;
        }

        const char ch = line[index];
        if (ch == '\t') {
            rendered.append("    ");
        } else if (static_cast<unsigned char>(ch) < 32) {
            rendered.push_back('?');
        } else {
            rendered.push_back(ch);
        }
    }
    if (inverted) {
        rendered += "\x1b[27m";
    }
    if (active_bold) {
        rendered += "\x1b[22m";
    }
    if (active_color_code != ResetColorCode()) {
        rendered += std::string(ResetColorCode());
    }
    if (next_delimiter_stack != nullptr) {
        *next_delimiter_stack = std::move(delimiter_stack);
    }
    return rendered;
}

std::string ActiveViewLabel(ViewKind view) {
    switch (view) {
        case ViewKind::File:
            return "FILE";
        case ViewKind::AiScratch:
            return "AI";
        case ViewKind::PatchPreview:
            return "PATCH";
        case ViewKind::BuildOutput:
            return "BUILD";
    }
    return "FILE";
}

}  // namespace

size_t Screen::ContentColumns(const EditorState& state, int total_cols) const {
    const size_t gutter_width = ShowsLineNumbers(state) ? GutterWidth(state.activeBuffer()) : 0;
    if (total_cols <= 0) {
        return 1;
    }
    return std::max<size_t>(1, static_cast<size_t>(total_cols) > gutter_width
                                   ? static_cast<size_t>(total_cols) - gutter_width
                                   : 1);
}

std::string Screen::Render(const EditorState& state,
                           const RenderOptions& options,
                           int rows,
                           int cols) const {
    const Buffer& buffer = state.activeBuffer();
    const Viewport& viewport = state.activeViewport();
    const int content_rows = std::max(1, rows - 2);
    const size_t gutter_width = ShowsLineNumbers(state) ? GutterWidth(buffer) : 0;
    const size_t content_cols = ContentColumns(state, cols);
    std::ostringstream output;
    output << "\x1b[?25l";
    output << "\x1b[H";

    const ISyntaxHighlighter& highlighter = HighlighterForLanguage(state.fileBuffer().languageId());
    const GitLineStatus* git_status = ShowsLineNumbers(state) ? &GitStatusForBuffer(buffer) : nullptr;
    const std::optional<Cursor> matching_brace =
        state.activeView() == ViewKind::File ? FindMatchingBrace(state.fileBuffer(), highlighter, state.fileCursor())
                                             : std::nullopt;
    FileRenderState file_render_state{
        .syntax_state = highlighter.InitialState(),
    };
    AiDiffRenderState ai_diff_state;
    if (state.activeView() == ViewKind::File) {
        file_render_state = StateBeforeVisibleRow(buffer, highlighter, viewport.row_offset);
    } else if (state.activeView() == ViewKind::AiScratch || state.activeView() == ViewKind::PatchPreview) {
        ai_diff_state = StateBeforeVisibleAiRow(buffer, highlighter, viewport.row_offset);
    }

    auto end_screen_row = [&](int screen_row) {
        output << "\x1b[K";
        if (screen_row < content_rows - 1) {
            output << "\r\n";
        }
    };

    int screen_row = 0;
    size_t file_row = viewport.row_offset;
    while (screen_row < content_rows) {
        if (file_row >= buffer.lineCount()) {
            if (ShowsLineNumbers(state)) {
                output << std::string(LineNumberDigits(buffer), ' ') << kGutterSeparator << ' ';
            }
            output << "~";
        } else {
            if (ShowsLineNumbers(state)) {
                output << RenderLineNumber(file_row, buffer, *git_status);
            }
            const std::string line = EscapeLine(buffer.line(file_row));
            if (state.activeView() == ViewKind::File) {
                SyntaxLineState next_line_state = file_render_state.syntax_state;
                std::vector<char> next_delimiter_stack = file_render_state.delimiter_stack;
                output << RenderFileLine(state,
                                         highlighter,
                                         buffer.line(file_row),
                                         file_row,
                                         viewport.col_offset,
                                         content_cols,
                                         file_render_state.syntax_state,
                                         file_render_state.delimiter_stack,
                                         matching_brace.has_value() ? std::optional<Cursor>(state.fileCursor())
                                                                    : std::nullopt,
                                         matching_brace,
                                         &next_line_state,
                                         &next_delimiter_stack);
                file_render_state.syntax_state = next_line_state;
                file_render_state.delimiter_stack = std::move(next_delimiter_stack);
            } else if (state.activeView() == ViewKind::AiScratch) {
                AiDiffRenderState next_ai_diff_state = ai_diff_state;
                output << RenderAiScratchLine(buffer.line(file_row),
                                              viewport.col_offset,
                                              content_cols,
                                              highlighter,
                                              ai_diff_state,
                                              &next_ai_diff_state);
                ai_diff_state = next_ai_diff_state;
            } else if (state.activeView() == ViewKind::PatchPreview) {
                AiDiffRenderState next_ai_diff_state = ai_diff_state;
                output << RenderPatchPreviewLine(buffer.line(file_row),
                                                viewport.col_offset,
                                                content_cols,
                                                highlighter,
                                                ai_diff_state,
                                                &next_ai_diff_state);
                ai_diff_state = next_ai_diff_state;
            } else if (viewport.col_offset < line.size()) {
                std::string visible = line.substr(viewport.col_offset, content_cols);
                if (options.file_picker_mode && state.activeView() == ViewKind::BuildOutput &&
                    file_row == options.file_picker_selected + 1) {
                    output << "\x1b[7m" << visible << "\x1b[27m";
                } else {
                    output << visible;
                }
            }
        }
        end_screen_row(screen_row);
        ++screen_row;

        if (state.activeView() == ViewKind::File && file_row < buffer.lineCount()) {
            const std::vector<std::string>& previous_lines =
                ExpandedPreviousLinesForRow(state, *git_status, file_row);
            for (const std::string& previous_line : previous_lines) {
                if (screen_row >= content_rows) {
                    break;
                }
                output << RenderGitPreviousLine(buffer, previous_line, viewport.col_offset, content_cols);
                end_screen_row(screen_row);
                ++screen_row;
            }
        }
        ++file_row;
    }

    const Cursor& file_cursor = state.fileCursor();
    const std::string modified = state.fileBuffer().dirty() ? "modified" : "saved";
    std::string status = state.activeBuffer().name() + " | " + state.fileBuffer().guessLanguage() + " | " +
                         modified + " | AI " + state.aiProviderName() + " | " +
                         ActiveViewLabel(state.activeView()) + " | Ln " +
                         std::to_string(file_cursor.row + 1) + ", Col " + std::to_string(file_cursor.col + 1);
    if (!state.aiRequestState().empty()) {
        status += " | " + state.aiRequestState();
    }
    if (status.size() > static_cast<size_t>(cols)) {
        status.resize(static_cast<size_t>(cols));
    }

    output << "\r\n\x1b[7m" << status;
    if (status.size() < static_cast<size_t>(cols)) {
        output << std::string(static_cast<size_t>(cols) - status.size(), ' ');
    }
    output << "\x1b[m";

    output << "\r\n";
    if (options.command_mode) {
        std::string command_line = ":" + options.command_input;
        if (command_line.size() > static_cast<size_t>(cols)) {
            command_line = command_line.substr(command_line.size() - static_cast<size_t>(cols));
        }
        output << command_line;
    } else if (options.file_picker_mode) {
        std::string picker_line = "Open file: " + options.file_picker_query;
        if (picker_line.size() > static_cast<size_t>(cols)) {
            picker_line = picker_line.substr(picker_line.size() - static_cast<size_t>(cols));
        }
        output << picker_line;
    } else {
        const std::string message = state.statusText();
        if (!message.empty()) {
            output << message.substr(0, static_cast<size_t>(cols));
        }
    }
    output << "\x1b[K";

    size_t cursor_row = 1;
    size_t cursor_col = 1;
    if (options.command_mode) {
        cursor_row = static_cast<size_t>(rows);
        cursor_col = std::min(static_cast<size_t>(cols), options.command_input.size() + 2);
    } else if (options.file_picker_mode) {
        cursor_row = static_cast<size_t>(rows);
        cursor_col = std::min(static_cast<size_t>(cols), std::string("Open file: ").size() + options.file_picker_query.size() + 1);
    } else {
        size_t visual_cursor_row =
            state.activeViewport().cursor.row >= viewport.row_offset
                ? state.activeViewport().cursor.row - viewport.row_offset
                : 0;
        if (state.activeView() == ViewKind::File && git_status != nullptr &&
            state.activeViewport().cursor.row >= viewport.row_offset) {
            visual_cursor_row +=
                ExpandedGitRowsBefore(state, *git_status, viewport.row_offset, state.activeViewport().cursor.row);
        }
        cursor_row = std::min(static_cast<size_t>(content_rows), visual_cursor_row + 1);
        cursor_col =
            std::min(static_cast<size_t>(cols), gutter_width + state.activeViewport().cursor.col - viewport.col_offset + 1);
    }

    output << "\x1b[" << cursor_row << ";" << cursor_col << "H";
    output << "\x1b[?25h";
    return output.str();
}

}  // namespace patchwork
