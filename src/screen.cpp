#include "screen.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

#include "selection.h"

namespace patchwork {

namespace {

constexpr std::string_view kIncludeDirectiveColor = "\x1b[38;5;141m";
constexpr std::string_view kIncludeTargetColor = "\x1b[38;5;214m";
constexpr std::string_view kDefaultColor = "\x1b[39m";

enum class SyntaxColor {
    Default,
    IncludeDirective,
    IncludeTarget,
};

struct HighlightSpan {
    size_t start = 0;
    size_t end = 0;
    SyntaxColor color = SyntaxColor::Default;
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

bool IsCppLikeFile(const Buffer& buffer) {
    const std::string language = buffer.guessLanguage();
    return language == "C" || language == "C++" || language == "C/C++";
}

std::vector<HighlightSpan> IncludeHighlightSpans(const Buffer& buffer, const std::string& line) {
    if (!IsCppLikeFile(buffer)) {
        return {};
    }

    const size_t directive_start = line.find_first_not_of(" \t");
    if (directive_start == std::string::npos || line[directive_start] != '#') {
        return {};
    }

    constexpr std::string_view kIncludeDirective = "#include";
    if (line.compare(directive_start, kIncludeDirective.size(), kIncludeDirective) != 0) {
        return {};
    }

    const size_t directive_end = directive_start + kIncludeDirective.size();
    if (directive_end < line.size() && !std::isspace(static_cast<unsigned char>(line[directive_end]))) {
        return {};
    }

    std::vector<HighlightSpan> spans;
    spans.push_back({.start = directive_start, .end = directive_end, .color = SyntaxColor::IncludeDirective});

    const size_t header_start = line.find_first_not_of(" \t", directive_end);
    if (header_start == std::string::npos) {
        return spans;
    }

    char closing_delimiter = '\0';
    if (line[header_start] == '<') {
        closing_delimiter = '>';
    } else if (line[header_start] == '"') {
        closing_delimiter = '"';
    } else {
        return spans;
    }

    size_t header_end = line.find(closing_delimiter, header_start + 1);
    if (header_end == std::string::npos) {
        header_end = line.size();
    } else {
        ++header_end;
    }

    spans.push_back({.start = header_start, .end = header_end, .color = SyntaxColor::IncludeTarget});
    return spans;
}

SyntaxColor HighlightColorAt(const std::vector<HighlightSpan>& spans, size_t index) {
    for (const HighlightSpan& span : spans) {
        if (index >= span.start && index < span.end) {
            return span.color;
        }
    }
    return SyntaxColor::Default;
}

std::string_view ColorCode(SyntaxColor color) {
    switch (color) {
        case SyntaxColor::IncludeDirective:
            return kIncludeDirectiveColor;
        case SyntaxColor::IncludeTarget:
            return kIncludeTargetColor;
        case SyntaxColor::Default:
            return kDefaultColor;
    }
    return kDefaultColor;
}

std::string RenderFileLine(const EditorState& state, const std::string& line, size_t row, size_t col_offset, size_t cols) {
    std::string rendered;
    rendered.reserve(cols + 16);

    if (col_offset >= line.size()) {
        return rendered;
    }

    const size_t end = std::min(line.size(), col_offset + cols);
    const std::vector<HighlightSpan> highlights = IncludeHighlightSpans(state.fileBuffer(), line);
    bool inverted = false;
    SyntaxColor active_color = SyntaxColor::Default;
    for (size_t index = col_offset; index < end; ++index) {
        const bool selected = IsPositionSelected(state.selection(), row, index);
        if (selected && !inverted) {
            rendered += "\x1b[7m";
            inverted = true;
        } else if (!selected && inverted) {
            rendered += "\x1b[27m";
            inverted = false;
        }

        const SyntaxColor color = HighlightColorAt(highlights, index);
        if (color != active_color) {
            rendered += ColorCode(color);
            active_color = color;
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
    if (active_color != SyntaxColor::Default) {
        rendered += std::string(kDefaultColor);
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

std::string Screen::Render(const EditorState& state,
                           const RenderOptions& options,
                           int rows,
                           int cols) const {
    const Buffer& buffer = state.activeBuffer();
    const Viewport& viewport = state.activeViewport();
    const int content_rows = std::max(1, rows - 2);
    std::ostringstream output;
    output << "\x1b[?25l";
    output << "\x1b[H";

    for (int screen_row = 0; screen_row < content_rows; ++screen_row) {
        const size_t file_row = viewport.row_offset + static_cast<size_t>(screen_row);
        if (file_row >= buffer.lineCount()) {
            output << "~";
        } else {
            const std::string line = EscapeLine(buffer.line(file_row));
            if (state.activeView() == ViewKind::File) {
                output << RenderFileLine(state, buffer.line(file_row), file_row, viewport.col_offset, cols);
            } else if (viewport.col_offset < line.size()) {
                std::string visible = line.substr(viewport.col_offset, static_cast<size_t>(cols));
                if (state.activeView() == ViewKind::PatchPreview) {
                    visible = DecoratePatchLine(visible);
                }
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
            std::min(static_cast<size_t>(cols), state.activeViewport().cursor.col - viewport.col_offset + 1);
    }

    output << "\x1b[" << cursor_row << ";" << cursor_col << "H";
    output << "\x1b[?25h";
    return output.str();
}

}  // namespace patchwork
