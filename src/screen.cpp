#include "screen.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "selection.h"
#include "syntax/registry.h"
#include "syntax/theme.h"

namespace patchwork {

namespace {

constexpr std::string_view kGutterSeparator = "\xE2\x94\x82";

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

std::string RenderLineNumber(size_t row, const Buffer& buffer) {
    std::ostringstream output;
    output << std::setw(static_cast<int>(LineNumberDigits(buffer))) << (row + 1) << kGutterSeparator << ' ';
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

SyntaxLineState StateBeforeVisibleRow(const Buffer& buffer,
                                      const ISyntaxHighlighter& highlighter,
                                      size_t row) {
    SyntaxLineState state = highlighter.InitialState();
    std::vector<SyntaxSpan> scratch;
    for (size_t index = 0; index < row && index < buffer.lineCount(); ++index) {
        state = highlighter.HighlightLine(buffer.line(index), state, &scratch);
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
                           SyntaxLineState* next_line_state) {
    std::string rendered;
    rendered.reserve(cols + 16);

    std::vector<SyntaxSpan> highlights;
    const SyntaxLineState updated_state = highlighter.HighlightLine(line, line_state, &highlights);
    if (next_line_state != nullptr) {
        *next_line_state = updated_state;
    }

    if (col_offset >= line.size()) {
        return rendered;
    }

    const size_t end = std::min(line.size(), col_offset + cols);
    bool inverted = false;
    SyntaxTokenKind active_token_kind = SyntaxTokenKind::Default;
    for (size_t index = col_offset; index < end; ++index) {
        const bool selected = IsPositionSelected(state.selection(), row, index);
        if (selected && !inverted) {
            rendered += "\x1b[7m";
            inverted = true;
        } else if (!selected && inverted) {
            rendered += "\x1b[27m";
            inverted = false;
        }

        const SyntaxTokenKind token_kind = TokenKindAt(highlights, index);
        if (token_kind != active_token_kind) {
            rendered += ColorCodeForToken(token_kind);
            active_token_kind = token_kind;
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
    if (active_token_kind != SyntaxTokenKind::Default) {
        rendered += std::string(ResetColorCode());
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
    SyntaxLineState line_state = highlighter.InitialState();
    AiDiffRenderState ai_diff_state;
    if (state.activeView() == ViewKind::File) {
        line_state = StateBeforeVisibleRow(buffer, highlighter, viewport.row_offset);
    } else if (state.activeView() == ViewKind::AiScratch || state.activeView() == ViewKind::PatchPreview) {
        ai_diff_state = StateBeforeVisibleAiRow(buffer, highlighter, viewport.row_offset);
    }

    for (int screen_row = 0; screen_row < content_rows; ++screen_row) {
        const size_t file_row = viewport.row_offset + static_cast<size_t>(screen_row);
        if (file_row >= buffer.lineCount()) {
            if (ShowsLineNumbers(state)) {
                output << std::string(LineNumberDigits(buffer), ' ') << kGutterSeparator << ' ';
            }
            output << "~";
        } else {
            if (ShowsLineNumbers(state)) {
                output << RenderLineNumber(file_row, buffer);
            }
            const std::string line = EscapeLine(buffer.line(file_row));
            if (state.activeView() == ViewKind::File) {
                SyntaxLineState next_line_state = line_state;
                output << RenderFileLine(state,
                                         highlighter,
                                         buffer.line(file_row),
                                         file_row,
                                         viewport.col_offset,
                                         content_cols,
                                         line_state,
                                         &next_line_state);
                line_state = next_line_state;
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
                output << visible;
            }
        }
        output << "\x1b[K";
        if (screen_row < content_rows - 1) {
            output << "\r\n";
        }
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
    } else {
        cursor_row =
            std::min(static_cast<size_t>(content_rows), state.activeViewport().cursor.row - viewport.row_offset + 1);
        cursor_col =
            std::min(static_cast<size_t>(cols), gutter_width + state.activeViewport().cursor.col - viewport.col_offset + 1);
    }

    output << "\x1b[" << cursor_row << ";" << cursor_col << "H";
    output << "\x1b[?25h";
    return output.str();
}

}  // namespace patchwork
