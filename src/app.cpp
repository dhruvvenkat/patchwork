#include "app.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <sstream>

#include "ai/mock_client.h"
#include "command.h"
#include "cursor.h"
#include "diff.h"
#include "git_status.h"
#include "intellisense/completion.h"
#include "patch.h"
#include "selection.h"

namespace flowstate {

namespace {

constexpr size_t kContextLines = 3;
constexpr std::chrono::milliseconds kAiLoadingTick(120);

std::string JoinRange(const Buffer& buffer, size_t start, size_t end) {
    if (buffer.lineCount() == 0 || start >= buffer.lineCount() || start > end) {
        return {};
    }

    std::ostringstream output;
    for (size_t row = start; row <= end && row < buffer.lineCount(); ++row) {
        if (row > start) {
            output << '\n';
        }
        output << buffer.line(row);
    }
    return output.str();
}

size_t SelectionStartRow(const EditorState& state) {
    if (HasSelection(state.selection())) {
        return NormalizeSelection(state.selection()).start.row;
    }
    return state.fileCursor().row;
}

size_t SelectionEndRow(const EditorState& state) {
    if (HasSelection(state.selection())) {
        return NormalizeSelection(state.selection()).end.row;
    }
    return state.fileCursor().row;
}

std::string AiRequestStateLabel(AiRequestState state) {
    switch (state) {
        case AiRequestState::Connecting:
            return "CONNECTING";
        case AiRequestState::Streaming:
            return "STREAMING";
        case AiRequestState::ParsingPatch:
            return "PARSING PATCH";
        case AiRequestState::Failed:
            return "FAILED";
        case AiRequestState::Complete:
            return "COMPLETE";
    }
    return {};
}

std::string CollapseStatusText(const std::string& text) {
    std::string collapsed;
    collapsed.reserve(text.size());

    bool last_was_space = false;
    for (char ch : text) {
        const bool is_space = (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ');
        if (is_space) {
            if (!collapsed.empty() && !last_was_space) {
                collapsed.push_back(' ');
            }
            last_was_space = true;
            continue;
        }

        collapsed.push_back(ch);
        last_was_space = false;
    }

    while (!collapsed.empty() && collapsed.front() == ' ') {
        collapsed.erase(collapsed.begin());
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::string AiFailureStatus(const std::string& error_message, bool backgrounded) {
    std::string status = CollapseStatusText(error_message);
    if (status.empty()) {
        status = "AI request failed.";
    }
    if (backgrounded) {
        status += " Press Alt+E to reopen AI scratch.";
    }
    return status;
}

std::optional<size_t> ParseOneBasedLineNumber(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    size_t parsed = 0;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::string Lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const unsigned char ch : text) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

bool IsFilePickerMatch(std::string_view path, std::string_view query) {
    if (query.empty()) {
        return true;
    }
    return Lowercase(path).find(Lowercase(query)) != std::string::npos;
}

bool IsSkippedPickerDirectory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == "build" || name == ".cache" || name == "node_modules";
}

std::string ScaffoldedPickerPath(std::string_view path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return std::string(path);
    }

    size_t depth = 0;
    for (size_t index = 0; index < slash; ++index) {
        if (path[index] == '/') {
            ++depth;
        }
    }
    return std::string((depth + 1) * 2, ' ') + std::string(path.substr(slash + 1));
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    if (path.empty()) {
        return std::filesystem::current_path();
    }
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::optional<std::filesystem::path> FindContainingGitRoot(std::filesystem::path path) {
    path = AbsolutePath(path);
    if (!std::filesystem::is_directory(path)) {
        path = path.parent_path();
    }

    while (!path.empty()) {
        if (std::filesystem::exists(path / ".git")) {
            return path;
        }
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return std::nullopt;
}

size_t ExpandedGitRowsBetween(const EditorState& state,
                              const GitLineStatus& git_status,
                              size_t start_row,
                              size_t end_row) {
    size_t count = 0;
    for (size_t row = start_row; row < end_row && row < git_status.lines.size(); ++row) {
        if (state.isGitChangePeekExpanded(row)) {
            count += git_status.lines[row].previous_lines.size();
        }
    }
    return count;
}

}  // namespace

EditorApp::EditorApp(Buffer file_buffer,
                     std::unique_ptr<IAiClient> ai_client,
                     std::string build_command,
                     std::string ai_provider_name)
    : state_(std::move(file_buffer)), ai_client_(std::move(ai_client)) {
    state_.setBuildCommand(std::move(build_command));
    state_.setAiProviderName(std::move(ai_provider_name));
    state_.setStatus("Alt+C commands, Ctrl+F finds, Ctrl+G selects, Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes.");
}

int EditorApp::Run() {
    std::string terminal_error;
    if (!terminal_.EnableRawMode(&terminal_error)) {
        terminal_.Write("Failed to initialize terminal raw mode: " + terminal_error + "\n");
        return 1;
    }

    while (running_) {
        PollAiRequest();
        PollCompletionRequest();
        RefreshScreen();
        const KeyPress key = terminal_.ReadKey();
        PollAiRequest();
        PollCompletionRequest();
        if (key.type == KeyType::Unknown) {
            continue;
        }
        if (command_mode_) {
            HandleCommandKey(key);
        } else if (file_picker_mode_) {
            HandleFilePickerKey(key);
        } else {
            HandleNormalKey(key);
        }
    }

    terminal_.Write("\x1b[2J\x1b[H");
    return 0;
}

void EditorApp::RefreshScreen() {
    const auto [screen_rows, screen_cols] = terminal_.WindowSize();
    ScrollToCursor(screen_rows, screen_cols);
    terminal_.Write(screen_.Render(state_,
                                   {.command_mode = command_mode_,
                                    .command_input = command_input_,
                                    .file_picker_mode = file_picker_mode_,
                                    .file_picker_query = file_picker_query_,
                                    .file_picker_selected = file_picker_selected_},
                                   screen_rows,
                                   screen_cols));
}

void EditorApp::ScrollToCursor(int screen_rows, int screen_cols) {
    Viewport& viewport = state_.activeViewport();
    const int content_rows = std::max(1, screen_rows - 2);
    const size_t content_cols = screen_.ContentColumns(state_, screen_cols);

    if (viewport.cursor.row < viewport.row_offset) {
        viewport.row_offset = viewport.cursor.row;
    }
    if (viewport.cursor.row >= viewport.row_offset + static_cast<size_t>(content_rows)) {
        viewport.row_offset = viewport.cursor.row - static_cast<size_t>(content_rows) + 1;
    }
    if (state_.activeView() == ViewKind::File && state_.hasGitChangePeekExpansions()) {
        const GitLineStatus git_status =
            LoadGitLineStatus(state_.fileBuffer().path().value_or(std::filesystem::path()),
                              state_.fileBuffer().lineCount());
        while (viewport.cursor.row > viewport.row_offset &&
               viewport.cursor.row - viewport.row_offset +
                       ExpandedGitRowsBetween(state_, git_status, viewport.row_offset, viewport.cursor.row) >=
                   static_cast<size_t>(content_rows)) {
            ++viewport.row_offset;
        }
    }
    if (viewport.cursor.col < viewport.col_offset) {
        viewport.col_offset = viewport.cursor.col;
    }
    if (viewport.cursor.col >= viewport.col_offset + content_cols) {
        viewport.col_offset = viewport.cursor.col - content_cols + 1;
    }
}

void EditorApp::HandleNormalKey(const KeyPress& key) {
    if (!(key.ctrl && key.ch == 'q')) {
        pending_quit_confirm_ = false;
    }

    if (state_.completionSession().active && HandleCompletionKey(key)) {
        return;
    }

    if (key.ctrl) {
        switch (key.ch) {
            case 'q':
                if (state_.fileBuffer().dirty() && !pending_quit_confirm_) {
                    pending_quit_confirm_ = true;
                    state_.setStatus("Unsaved changes. Press Ctrl+Q again to quit.");
                } else {
                    QuitEditor();
                }
                return;
            case 's':
                SaveFile();
                return;
            case 'c':
                CopySelectionOrLine();
                return;
            case 'e':
                RunAiRequest(AiRequestKind::Explain, "Explain this code.");
                return;
            case 'f':
                StartFindPrompt();
                return;
            case 'o':
                StartFilePicker();
                return;
            case 'r':
                RunAiRequest(AiRequestKind::Fix, "Fix bugs in this code and keep the patch minimal.");
                return;
            case 't':
                RunBuild();
                return;
            case 'v':
                PasteClipboard();
                return;
            case 'x':
                CutSelectionOrLine();
                return;
            case 'y':
                RedoFileEdit();
                return;
            case 'z':
                UndoFileEdit();
                return;
            case 'g':
                ToggleSelection();
                return;
        }
    }

    if (key.alt) {
        if (key.ch == 'a') {
            HandlePatchAction(CommandType::PatchAccept);
            return;
        }
        if (key.ch == 'c') {
            StartCommandPrompt();
            return;
        }
        if (key.ch == 'd') {
            ToggleGitPreviousLines();
            return;
        }
        if (key.ch == 'e') {
            ReopenAiScratch();
            return;
        }
        if (key.ch == 'i') {
            RequestCompletion(false);
            return;
        }
        if (key.ch == 'p') {
            ReopenPatchPreview();
            return;
        }
        if (key.ch == 'r') {
            HandlePatchAction(CommandType::PatchReject);
            return;
        }
    }

    switch (key.type) {
        case KeyType::ArrowLeft:
        case KeyType::ArrowRight:
        case KeyType::ArrowUp:
        case KeyType::ArrowDown:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelection(key.type);
                return;
            }
            MoveCursor(key.type);
            return;
        case KeyType::Home:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelectionToLineBoundary(key.type);
                return;
            }
            CursorController::moveHome(state_.activeViewport().cursor);
            UpdateSelectionHead();
            return;
        case KeyType::End:
            if (key.shift && state_.activeView() == ViewKind::File) {
                ExtendSelectionToLineBoundary(key.type);
                return;
            }
            CursorController::moveEnd(state_.activeViewport().cursor, state_.activeBuffer());
            UpdateSelectionHead();
            return;
        case KeyType::PageUp:
            MoveCursor(KeyType::ArrowUp, 10);
            return;
        case KeyType::PageDown:
            MoveCursor(KeyType::ArrowDown, 10);
            return;
        case KeyType::Escape:
            if (state_.activeView() != ViewKind::File) {
                state_.setActiveView(ViewKind::File);
                if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
                    ai_request_backgrounded_ = true;
                    state_.setStatus("Returned to file buffer. AI request continues in background.", 60);
                } else {
                    state_.setStatus("Returned to file buffer.");
                }
            } else if (state_.selection().active) {
                state_.clearSelection();
                state_.setStatus("Selection cleared.");
            }
            return;
        case KeyType::Backspace:
            if (state_.activeView() == ViewKind::File) {
                if (DeleteSelectionIfActive()) {
                    return;
                }
                state_.BeginFileEdit();
                state_.fileBuffer().deleteCharBefore(state_.fileCursor());
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::DeleteKey:
            if (state_.activeView() == ViewKind::File) {
                if (DeleteSelectionIfActive()) {
                    return;
                }
                state_.BeginFileEdit();
                state_.fileBuffer().deleteCharAt(state_.fileCursor());
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Tab:
            if (state_.activeView() == ViewKind::File) {
                state_.BeginFileEdit();
                state_.fileBuffer().insertIndent(state_.fileCursor());
                UpdateSelectionHead();
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Enter:
            if (state_.activeView() == ViewKind::File) {
                state_.BeginFileEdit();
                state_.fileBuffer().insertNewline(state_.fileCursor());
                UpdateSelectionHead();
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                }
            }
            return;
        case KeyType::Character:
            if (state_.activeView() == ViewKind::File && !key.ctrl &&
                static_cast<unsigned char>(key.ch) >= 32) {
                state_.BeginFileEdit();
                state_.fileBuffer().insertChar(state_.fileCursor(), key.ch);
                ++state_.fileCursor().col;
                UpdateSelectionHead();
                if (state_.CommitFileEdit()) {
                    NotifyCompletionDocumentChanged();
                    InvalidatePatchSessionForManualFileEdit();
                    if (IsCompletionAutoTrigger(state_.fileBuffer(), state_.fileCursor())) {
                        RequestCompletion(true);
                    }
                }
            }
            return;
        case KeyType::Unknown:
            return;
    }
}

void EditorApp::HandleCommandKey(const KeyPress& key) {
    switch (key.type) {
        case KeyType::Escape:
            command_mode_ = false;
            command_input_.clear();
            state_.setStatus("Command cancelled.");
            return;
        case KeyType::Backspace:
            if (!command_input_.empty()) {
                command_input_.pop_back();
            }
            return;
        case KeyType::Enter: {
            command_mode_ = false;
            const Command command = ParseCommand(command_input_);
            command_input_.clear();
            if (!ExecuteCommand(command)) {
                state_.setStatus(command.error.empty() ? "Command failed." : command.error);
            }
            return;
        }
        case KeyType::Character:
            if (!key.ctrl && static_cast<unsigned char>(key.ch) >= 32) {
                command_input_.push_back(key.ch);
            }
            return;
        default:
            return;
    }
}

void EditorApp::HandleFilePickerKey(const KeyPress& key) {
    switch (key.type) {
        case KeyType::Escape:
            file_picker_mode_ = false;
            file_picker_query_.clear();
            file_picker_matches_.clear();
            state_.setActiveView(ViewKind::File);
            state_.setStatus("File picker cancelled.");
            return;
        case KeyType::Backspace:
            if (!file_picker_query_.empty()) {
                file_picker_query_.pop_back();
                file_picker_selected_ = 0;
                RefreshFilePickerMatches();
            }
            return;
        case KeyType::Enter:
            if (file_picker_matches_.empty()) {
                state_.setStatus("No file selected.");
                return;
            } else {
                const std::filesystem::path path = file_picker_root_ / file_picker_matches_[file_picker_selected_];
                file_picker_mode_ = false;
                file_picker_query_.clear();
                file_picker_matches_.clear();
                OpenFile(path.string());
            }
            return;
        case KeyType::ArrowUp:
            if (file_picker_selected_ > 0) {
                --file_picker_selected_;
                state_.activeViewport().cursor.row = file_picker_selected_ + 1;
            }
            return;
        case KeyType::ArrowDown:
            if (file_picker_selected_ + 1 < file_picker_matches_.size()) {
                ++file_picker_selected_;
                state_.activeViewport().cursor.row = file_picker_selected_ + 1;
            }
            return;
        case KeyType::Character:
            if (!key.ctrl && static_cast<unsigned char>(key.ch) >= 32) {
                file_picker_query_.push_back(key.ch);
                file_picker_selected_ = 0;
                RefreshFilePickerMatches();
            }
            return;
        default:
            return;
    }
}

void EditorApp::MoveCursor(KeyType key, size_t distance) {
    for (size_t step = 0; step < distance; ++step) {
        switch (key) {
            case KeyType::ArrowLeft:
                CursorController::moveLeft(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowRight:
                CursorController::moveRight(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowUp:
                CursorController::moveUp(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            case KeyType::ArrowDown:
                CursorController::moveDown(state_.activeViewport().cursor, state_.activeBuffer());
                break;
            default:
                break;
        }
    }

    if (state_.activeView() == ViewKind::PatchPreview && state_.patchSession().has_value()) {
        PatchSession& session = *state_.patchSession();
        session.current_hunk = HunkIndexForPreviewRow(session, state_.viewport(ViewKind::PatchPreview).cursor.row);
    }
    UpdateSelectionHead();
}

void EditorApp::ExtendSelection(KeyType key) {
    if (!state_.selection().active) {
        state_.selection().active = true;
        state_.selection().extend_on_cursor_move = false;
        state_.selection().anchor = state_.fileCursor();
        state_.selection().head = state_.fileCursor();
    }
    switch (key) {
        case KeyType::ArrowLeft:
            CursorController::moveLeft(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowRight:
            CursorController::moveRight(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowUp:
            CursorController::moveUp(state_.fileCursor(), state_.fileBuffer());
            break;
        case KeyType::ArrowDown:
            CursorController::moveDown(state_.fileCursor(), state_.fileBuffer());
            break;
        default:
            break;
    }
    state_.selection().head = state_.fileCursor();
}

void EditorApp::ExtendSelectionToLineBoundary(KeyType key) {
    if (!state_.selection().active) {
        state_.selection().active = true;
        state_.selection().extend_on_cursor_move = false;
        state_.selection().anchor = state_.fileCursor();
        state_.selection().head = state_.fileCursor();
    }

    if (key == KeyType::Home) {
        CursorController::moveHome(state_.fileCursor());
    } else if (key == KeyType::End) {
        CursorController::moveEnd(state_.fileCursor(), state_.fileBuffer());
    }
    state_.selection().head = state_.fileCursor();
}

void EditorApp::UpdateSelectionHead() {
    if (state_.activeView() != ViewKind::File || !state_.selection().active) {
        return;
    }
    if (state_.selection().extend_on_cursor_move) {
        state_.selection().head = state_.fileCursor();
    } else {
        state_.clearSelection();
    }
}

bool EditorApp::ExecuteCommand(const Command& command) {
    switch (command.type) {
        case CommandType::Open:
            return OpenFile(command.argument);
        case CommandType::Write:
            SaveFile();
            return true;
        case CommandType::Quit:
            if (state_.fileBuffer().dirty() && !pending_quit_confirm_) {
                pending_quit_confirm_ = true;
                state_.setStatus("Unsaved changes. Press :quit again to discard them.");
            } else {
                QuitEditor();
            }
            return true;
        case CommandType::Build:
            RunBuild();
            return true;
        case CommandType::Find:
            return FindText(command.argument);
        case CommandType::Goto:
            return GotoLine(command.argument);
        case CommandType::AiExplain:
            RunAiRequest(AiRequestKind::Explain, "Explain this code.");
            return true;
        case CommandType::AiFix:
            RunAiRequest(AiRequestKind::Fix, "Fix bugs in this code and keep the patch minimal.");
            return true;
        case CommandType::AiRefactor:
            RunAiRequest(AiRequestKind::Refactor, "Refactor this code without changing behavior.");
            return true;
        case CommandType::AiError:
            RunAiRequest(AiRequestKind::ErrorExplain, "Explain the latest build failure.");
            return true;
        case CommandType::PatchAccept:
        case CommandType::PatchReject:
        case CommandType::PatchAcceptAll:
        case CommandType::PatchRejectAll:
            HandlePatchAction(command.type);
            return true;
        case CommandType::Invalid:
            return false;
    }
    return false;
}

bool EditorApp::OpenFile(const std::string& path) {
    std::string error;
    Buffer buffer = LoadFileBuffer(std::filesystem::path(path), &error);
    if (!error.empty()) {
        state_.setStatus(error);
        return false;
    }
    state_.setFileBuffer(std::move(buffer));
    state_.clearCompletionSession();
    state_.setStatus("Opened " + path + ".");
    return true;
}

bool EditorApp::FindText(const std::string& query) {
    if (query.empty()) {
        state_.setStatus("Usage: :find <text>");
        return false;
    }

    const Buffer& buffer = state_.fileBuffer();
    if (buffer.lineCount() == 0) {
        state_.setStatus("No match for \"" + query + "\".");
        return false;
    }

    const Cursor cursor = state_.fileCursor();
    const size_t start_row = std::min(cursor.row, buffer.lineCount() - 1);
    const size_t start_col = std::min(cursor.col + 1, buffer.line(start_row).size());

    auto find_from = [&](size_t row, size_t col) -> std::optional<Cursor> {
        const size_t found = buffer.line(row).find(query, col);
        if (found == std::string::npos) {
            return std::nullopt;
        }
        return Cursor{row, found};
    };

    std::optional<Cursor> match;
    for (size_t row = start_row; row < buffer.lineCount(); ++row) {
        match = find_from(row, row == start_row ? start_col : 0);
        if (match.has_value()) {
            break;
        }
    }
    if (!match.has_value()) {
        for (size_t row = 0; row <= start_row && row < buffer.lineCount(); ++row) {
            match = find_from(row, 0);
            if (match.has_value()) {
                break;
            }
        }
    }

    if (!match.has_value()) {
        state_.setStatus("No match for \"" + query + "\".");
        return false;
    }

    state_.setActiveView(ViewKind::File);
    state_.fileCursor() = *match;
    state_.selection().active = true;
    state_.selection().extend_on_cursor_move = false;
    state_.selection().anchor = *match;
    state_.selection().head = Cursor{match->row, match->col + query.size()};
    state_.setStatus("Found \"" + query + "\" at line " + std::to_string(match->row + 1) + ".");
    return true;
}

bool EditorApp::GotoLine(const std::string& line_text) {
    const std::optional<size_t> line_number = ParseOneBasedLineNumber(line_text);
    if (!line_number.has_value()) {
        state_.setStatus("Usage: :goto <line>");
        return false;
    }
    if (*line_number > state_.fileBuffer().lineCount()) {
        state_.setStatus("Line out of range. File has " + std::to_string(state_.fileBuffer().lineCount()) + " lines.");
        return false;
    }

    state_.setActiveView(ViewKind::File);
    state_.fileCursor().row = *line_number - 1;
    state_.fileCursor().col = 0;
    state_.clearSelection();
    state_.setStatus("Jumped to line " + std::to_string(*line_number) + ".");
    return true;
}

void EditorApp::StartCommandPrompt() {
    command_mode_ = true;
    command_input_.clear();
    state_.setStatus("Command mode.");
}

void EditorApp::StartFindPrompt() {
    command_mode_ = true;
    command_input_ = "find ";
    state_.setStatus("Find in file.");
}

void EditorApp::StartFilePicker() {
    file_picker_root_ = FilePickerRoot();
    file_picker_files_ = DiscoverFilePickerFiles();
    file_picker_query_.clear();
    file_picker_mode_ = true;
    command_mode_ = false;
    state_.buildBuffer().setName("File Picker");
    state_.setActiveView(ViewKind::BuildOutput);
    RefreshFilePickerMatches();
    state_.setStatus("Type to filter, arrows select, Enter opens, Esc cancels.", 60);
}

std::filesystem::path EditorApp::FilePickerRoot() const {
    const std::filesystem::path active_path =
        state_.fileBuffer().path().has_value() ? AbsolutePath(*state_.fileBuffer().path()) : std::filesystem::current_path();
    if (const std::optional<std::filesystem::path> git_root = FindContainingGitRoot(active_path)) {
        return *git_root;
    }
    if (std::filesystem::is_directory(active_path)) {
        return active_path;
    }
    if (active_path.has_parent_path()) {
        return active_path.parent_path();
    }
    return std::filesystem::current_path();
}

std::vector<std::string> EditorApp::DiscoverFilePickerFiles() const {
    std::vector<std::string> files;
    const std::filesystem::path root = file_picker_root_.empty() ? FilePickerRoot() : file_picker_root_;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::directory_entry entry = *iterator;
        std::error_code entry_error;
        if (entry.is_directory(entry_error) && IsSkippedPickerDirectory(entry.path())) {
            iterator.disable_recursion_pending();
        } else if (entry.is_regular_file(entry_error)) {
            std::error_code relative_error;
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), root, relative_error);
            if (!relative_error) {
                files.push_back(relative.generic_string());
            }
        }
        iterator.increment(error);
    }
    std::sort(files.begin(), files.end());
    return files;
}

void EditorApp::RefreshFilePickerMatches() {
    file_picker_matches_.clear();
    for (const std::string& path : file_picker_files_) {
        if (IsFilePickerMatch(path, file_picker_query_)) {
            file_picker_matches_.push_back(path);
        }
        if (file_picker_matches_.size() >= 200) {
            break;
        }
    }
    file_picker_selected_ = std::min(file_picker_selected_,
                                     file_picker_matches_.empty() ? size_t{0} : file_picker_matches_.size() - 1);
    std::vector<std::string> lines;
    lines.push_back("Root: " + file_picker_root_.string());
    if (file_picker_matches_.empty()) {
        lines.push_back("No files match \"" + file_picker_query_ + "\".");
    } else {
        lines.reserve(file_picker_matches_.size() + 1);
        for (const std::string& path : file_picker_matches_) {
            lines.push_back(ScaffoldedPickerPath(path));
        }
    }
    state_.buildBuffer().setName("File Picker");
    state_.buildBuffer().setLines(std::move(lines), false);
    state_.buildBuffer().clearDirty();
    state_.viewport(ViewKind::BuildOutput).cursor.row = file_picker_selected_ + 1;
    state_.viewport(ViewKind::BuildOutput).cursor.col = 0;
}

void EditorApp::SaveFile() {
    std::string error;
    if (state_.fileBuffer().save(&error)) {
        state_.setStatus("Saved " + state_.fileBuffer().name() + ".");
    } else {
        state_.setStatus(error.empty() ? "Save failed." : error);
    }
}

void EditorApp::ToggleSelection() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Selection only works in the file buffer.");
        return;
    }

    if (!state_.selection().active) {
        state_.selection().active = true;
        state_.selection().extend_on_cursor_move = true;
        state_.selection().anchor = state_.fileCursor();
        state_.selection().head = state_.fileCursor();
        state_.setStatus("Selection started.");
    } else {
        state_.clearSelection();
        state_.setStatus("Selection cleared.");
    }
}

void EditorApp::CopySelectionOrLine() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Copy only works in the file buffer.");
        return;
    }

    if (HasSelection(state_.selection())) {
        state_.setClipboardText(ExtractSelection(state_.fileBuffer(), state_.selection()));
        state_.setStatus("Copied selection.");
        return;
    }

    const SelectionRange range = CurrentLineRange(state_.fileBuffer(), state_.fileCursor());
    state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
    state_.setStatus("Copied line.");
}

void EditorApp::CutSelectionOrLine() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Cut only works in the file buffer.");
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        state_.setStatus("Buffer is read-only.");
        return;
    }

    Cursor& cursor = state_.fileCursor();
    state_.BeginFileEdit();
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
        state_.fileBuffer().deleteRange(cursor, range.start, range.end);
        state_.clearSelection();
        if (state_.CommitFileEdit()) {
            NotifyCompletionDocumentChanged();
            InvalidatePatchSessionForManualFileEdit();
            state_.setStatus("Cut selection.");
        }
        return;
    }

    const SelectionRange range = CurrentLineRange(state_.fileBuffer(), cursor);
    state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
    state_.fileBuffer().deleteRange(cursor, range.start, range.end);
    cursor.col = 0;
    CursorController::clamp(cursor, state_.fileBuffer());
    state_.clearSelection();
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
        state_.setStatus("Cut line.");
    }
}

void EditorApp::PasteClipboard() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Paste only works in the file buffer.");
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        state_.setStatus("Buffer is read-only.");
        return;
    }
    if (!state_.hasClipboardText()) {
        state_.setStatus("Clipboard is empty.");
        return;
    }

    Cursor& cursor = state_.fileCursor();
    state_.BeginFileEdit();
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.fileBuffer().replaceRange(cursor, range.start, range.end, state_.clipboardText());
    } else {
        state_.fileBuffer().insertText(cursor, state_.clipboardText());
    }

    state_.clearSelection();
    CursorController::clamp(cursor, state_.fileBuffer());
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
        state_.setStatus("Pasted clipboard.");
    }
}

void EditorApp::ToggleGitPreviousLines() {
    if (state_.activeView() != ViewKind::File) {
        state_.setStatus("Git change peek only works in the file buffer.");
        return;
    }

    const size_t row = state_.fileCursor().row;
    const GitLineStatus git_status =
        LoadGitLineStatus(state_.fileBuffer().path().value_or(std::filesystem::path()), state_.fileBuffer().lineCount());
    if (!git_status.available || row >= git_status.lines.size() || git_status.lines[row].previous_lines.empty()) {
        state_.setStatus("No previous lines at this row.");
        return;
    }

    const bool was_expanded = state_.isGitChangePeekExpanded(row);
    state_.toggleGitChangePeekExpansion(row);
    state_.setStatus((was_expanded ? "Hid " : "Showing ") +
                     std::to_string(git_status.lines[row].previous_lines.size()) +
                     " previous line" + (git_status.lines[row].previous_lines.size() == 1 ? "." : "s."));
}

bool EditorApp::DeleteSelectionIfActive() {
    if (!HasSelection(state_.selection())) {
        return false;
    }

    Cursor& cursor = state_.fileCursor();
    const SelectionRange range = NormalizeSelection(state_.selection());
    state_.BeginFileEdit();
    state_.fileBuffer().deleteRange(cursor, range.start, range.end);
    state_.clearSelection();
    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }
    state_.setStatus("Deleted selection.");
    return true;
}

void EditorApp::UndoFileEdit() {
    if (state_.activeView() != ViewKind::File && state_.activeView() != ViewKind::PatchPreview) {
        state_.setStatus("Undo only works on the file buffer.");
        return;
    }
    if (!state_.UndoFileEdit()) {
        state_.setStatus("Nothing to undo.");
        return;
    }

    state_.setPatchSession(std::nullopt);
    state_.setActiveView(ViewKind::File);
    NotifyCompletionDocumentChanged();
    state_.setStatus("Undid last file change.");
}

void EditorApp::RedoFileEdit() {
    if (state_.activeView() != ViewKind::File && state_.activeView() != ViewKind::PatchPreview) {
        state_.setStatus("Redo only works on the file buffer.");
        return;
    }
    if (!state_.RedoFileEdit()) {
        state_.setStatus("Nothing to redo.");
        return;
    }

    state_.setPatchSession(std::nullopt);
    state_.setActiveView(ViewKind::File);
    NotifyCompletionDocumentChanged();
    state_.setStatus("Redid last file change.");
}

void EditorApp::RunBuild() {
    state_.buildBuffer().setName("Build Output");
    const BuildResult result = RunBuildCommand(state_.buildCommand());
    state_.setLastBuild(result);

    std::ostringstream output;
    output << "$ " << state_.buildCommand() << "\n";
    if (!result.output.empty()) {
        output << result.output;
        if (!result.output.empty() && result.output.back() != '\n') {
            output << '\n';
        }
    }
    output << "[exit " << result.exit_code << "]";

    state_.setBuildOutput(output.str());
    state_.setActiveView(ViewKind::BuildOutput);
    state_.setStatus("Build finished with exit code " + std::to_string(result.exit_code) + ".");
}

AiRequest EditorApp::BuildAiRequest(AiRequestKind kind, const std::string& instruction) const {
    const Buffer& file = state_.fileBuffer();
    const size_t start_row = SelectionStartRow(state_);
    const size_t end_row = SelectionEndRow(state_);
    const size_t before_start = (start_row > kContextLines) ? start_row - kContextLines : 0;
    const size_t after_end = std::min(file.lineCount() - 1, end_row + kContextLines);

    AiRequest request;
    request.kind = kind;
    request.file_path = file.path().has_value() ? file.path()->string() : file.name();
    request.language = file.guessLanguage();
    request.cursor = state_.fileCursor();
    request.selected_text = state_.selectedOrCurrentText();
    request.context_before = (start_row > 0) ? JoinRange(file, before_start, start_row - 1) : "";
    request.context_after =
        (end_row + 1 < file.lineCount()) ? JoinRange(file, end_row + 1, after_end) : "";
    request.user_instruction = instruction;
    request.build_command = state_.buildCommand();
    request.build_output = state_.lastBuild().has_value() ? state_.lastBuild()->output : "";
    return request;
}

void EditorApp::RunAiRequest(AiRequestKind kind, std::string instruction) {
    if (active_ai_request_.has_value() || ai_client_->HasActiveRequest()) {
        state_.setStatus("AI request already in progress.", 60);
        return;
    }
    if (kind == AiRequestKind::ErrorExplain && !state_.lastBuild().has_value()) {
        state_.setStatus("Run :build before :ai error.");
        return;
    }

    const AiRequest request = BuildAiRequest(kind, instruction);
    std::string action = "AI request";
    switch (kind) {
        case AiRequestKind::Explain:
            action = "Explaining selection";
            break;
        case AiRequestKind::Fix:
            action = "Generating fix patch";
            break;
        case AiRequestKind::Refactor:
            action = "Generating refactor patch";
            break;
        case AiRequestKind::ErrorExplain:
            action = "Explaining build error";
            break;
    }

    state_.setPatchSession(std::nullopt);
    state_.setActiveView(ViewKind::AiScratch);
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Connecting));
    state_.setStatus(action + " via " + state_.aiProviderName() + "...", 3600);
    active_ai_request_ = ActiveAiRequest{.kind = kind, .label = action, .streamed_text = ""};
    ai_request_backgrounded_ = false;
    ai_loading_frame_ = 0;
    next_ai_loading_tick_ = std::chrono::steady_clock::now();
    RenderActiveAiScratch();

    std::string error;
    if (!ai_client_->StartRequest(request, &error)) {
        active_ai_request_.reset();
        ai_request_backgrounded_ = false;
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
        ShowAiText(error.empty() ? "AI request failed." : error, true);
        state_.setStatus(AiFailureStatus(error, false), 60);
    }
}

void EditorApp::PollAiRequest() {
    UpdateAiLoadingView();

    for (const AiEvent& event : ai_client_->PollEvents()) {
        switch (event.kind) {
            case AiEventKind::StateChanged:
                if (event.state == AiRequestState::Connecting || event.state == AiRequestState::Streaming) {
                    state_.setAiRequestState(AiRequestStateLabel(event.state));
                }
                break;
            case AiEventKind::TextDelta:
                if (active_ai_request_.has_value() && !event.text_delta.empty()) {
                    active_ai_request_->streamed_text += event.text_delta;
                    RenderActiveAiScratch();
                }
                break;
            case AiEventKind::Completed:
                HandleAiResponse(event.response);
                active_ai_request_.reset();
                break;
            case AiEventKind::Error:
                HandleAiError(event.error_message);
                break;
        }
    }
}

void EditorApp::PollCompletionRequest() {
    if (!state_.completionSession().active || !state_.completionSession().waiting) {
        clangd_client_.PollEvents();
        return;
    }

    for (const CompletionEvent& event : clangd_client_.PollEvents()) {
        CompletionSession session = state_.completionSession();
        if (event.request_id != 0 && event.request_id != session.request_id) {
            continue;
        }

        session.waiting = false;
        if (event.kind == CompletionEventKind::Error) {
            state_.clearCompletionSession();
            state_.setStatus(event.error_message.empty() ? "Completion request failed." : event.error_message, 10);
            return;
        }

        session.items = event.items;
        session.selected = 0;
        if (session.items.empty()) {
            state_.clearCompletionSession();
            state_.setStatus("No completions.", 3);
            return;
        }
        session.message.clear();
        state_.setCompletionSession(std::move(session));
    }
}

void EditorApp::RequestCompletion(bool automatic) {
    if (automatic && completion_auto_suppressed_) {
        return;
    }
    if (!automatic) {
        completion_auto_suppressed_ = false;
    }

    if (state_.activeView() != ViewKind::File) {
        if (!automatic) {
            state_.setStatus("Completion only works in the file buffer.");
        }
        return;
    }
    if (!IsCppCompletionLanguage(state_.fileBuffer().languageId())) {
        if (!automatic) {
            state_.setStatus("IntelliSense is only enabled for C++ right now.");
        }
        return;
    }

    Cursor cursor = state_.fileCursor();
    CursorController::clamp(cursor, state_.fileBuffer());
    const Cursor replace_start = CompletionPrefixStart(state_.fileBuffer(), cursor);

    std::string error;
    if (!clangd_client_.Start(ResolveClangdProjectRoot(state_.fileBuffer()), &error)) {
        state_.clearCompletionSession();
        if (automatic) {
            completion_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to start clangd." : error, 10);
        return;
    }
    if (!clangd_client_.SyncDocument(state_.fileBuffer(), &error)) {
        state_.clearCompletionSession();
        if (automatic) {
            completion_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to sync file with clangd." : error, 10);
        return;
    }

    const std::optional<int> request_id = clangd_client_.RequestCompletion(state_.fileBuffer(), cursor, &error);
    if (!request_id.has_value()) {
        state_.clearCompletionSession();
        if (automatic) {
            completion_auto_suppressed_ = true;
        }
        state_.setStatus(error.empty() ? "Unable to request completions." : error, 10);
        return;
    }

    state_.setCompletionSession({
        .active = true,
        .waiting = true,
        .request_id = *request_id,
        .replace_start = replace_start,
        .replace_end = cursor,
        .message = "Completing...",
    });
    if (!automatic) {
        state_.setStatus("Completing with clangd...", 3);
    }
}

void EditorApp::CloseCompletion() { state_.clearCompletionSession(); }

bool EditorApp::HandleCompletionKey(const KeyPress& key) {
    CompletionSession session = state_.completionSession();
    if (!session.active) {
        return false;
    }

    switch (key.type) {
        case KeyType::Escape:
            CloseCompletion();
            state_.setStatus("Completion cancelled.");
            return true;
        case KeyType::ArrowUp:
            if (!session.items.empty() && session.selected > 0) {
                --session.selected;
                state_.setCompletionSession(std::move(session));
            }
            return true;
        case KeyType::ArrowDown:
            if (!session.items.empty() && session.selected + 1 < session.items.size()) {
                ++session.selected;
                state_.setCompletionSession(std::move(session));
            }
            return true;
        case KeyType::Enter:
        case KeyType::Tab:
            AcceptCompletion();
            return true;
        case KeyType::Character:
            if (!key.ctrl) {
                CloseCompletion();
            }
            return false;
        case KeyType::Backspace:
        case KeyType::DeleteKey:
        case KeyType::Home:
        case KeyType::End:
        case KeyType::PageUp:
        case KeyType::PageDown:
        case KeyType::ArrowLeft:
        case KeyType::ArrowRight:
            CloseCompletion();
            return false;
        case KeyType::Unknown:
            return false;
    }
    return false;
}

void EditorApp::AcceptCompletion() {
    CompletionSession session = state_.completionSession();
    if (!session.active || session.waiting || session.items.empty() || session.selected >= session.items.size()) {
        CloseCompletion();
        return;
    }
    if (state_.fileBuffer().readOnly()) {
        CloseCompletion();
        state_.setStatus("Buffer is read-only.");
        return;
    }

    state_.BeginFileEdit();
    const CompletionItem item = session.items[session.selected];
    if (!ApplyCompletionItem(state_.fileBuffer(),
                             state_.fileCursor(),
                             item,
                             session.replace_start,
                             session.replace_end)) {
        state_.CommitFileEdit();
        CloseCompletion();
        state_.setStatus("Unable to apply completion.");
        return;
    }

    if (state_.CommitFileEdit()) {
        NotifyCompletionDocumentChanged();
        InvalidatePatchSessionForManualFileEdit();
    }
    state_.setStatus("Completed " + item.label + ".");
}

void EditorApp::NotifyCompletionDocumentChanged() {
    if (!clangd_client_.IsStarted() || !IsCppCompletionLanguage(state_.fileBuffer().languageId())) {
        return;
    }
    std::string ignored;
    clangd_client_.SyncDocument(state_.fileBuffer(), &ignored);
}

void EditorApp::UpdateAiLoadingView() {
    if (!active_ai_request_.has_value()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < next_ai_loading_tick_) {
        return;
    }

    RenderActiveAiScratch();
    next_ai_loading_tick_ = now + kAiLoadingTick;
    ++ai_loading_frame_;
}

void EditorApp::RenderActiveAiScratch() {
    if (!active_ai_request_.has_value()) {
        return;
    }

    static constexpr const char* kFrames[] = {"|", "/", "-", "\\"};
    const std::string header =
        active_ai_request_->label + " via " + state_.aiProviderName() + " " + kFrames[ai_loading_frame_ % 4];

    std::string text = header + "\n\n";
    if (active_ai_request_->streamed_text.empty()) {
        text += "Waiting for response.\nPress Esc to return to the file buffer.";
    } else {
        text += active_ai_request_->streamed_text;
    }
    ShowAiText(text, false);
}

void EditorApp::HandleAiResponse(const AiResponse& response) {
    if (response.kind == AiResponseKind::Error) {
        HandleAiError(response.error_message);
        return;
    }

    if (response.diff_text.has_value()) {
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::ParsingPatch));
        const PatchSet patch = ParseUnifiedDiff(*response.diff_text);
        if (!patch.valid()) {
            ShowAiText(response.raw_text, !ai_request_backgrounded_);
            state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
            state_.setStatus(ai_request_backgrounded_ ? "AI returned a malformed patch. Press Alt+E to reopen AI scratch."
                                                      : "AI returned a malformed patch.",
                             60);
            ai_request_backgrounded_ = false;
            return;
        }
        if (!PatchTargetsBuffer(patch, state_.fileBuffer())) {
            ShowAiText(response.raw_text, !ai_request_backgrounded_);
            state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
            state_.setStatus(ai_request_backgrounded_
                                 ? "AI patch does not target the current file. Press Alt+E to reopen AI scratch."
                                 : "AI patch does not target the current file.",
                             60);
            ai_request_backgrounded_ = false;
            return;
        }

        state_.setAiText(response.raw_text);
        state_.setPatchSession(CreatePatchSession(patch, state_.fileBuffer()));
        state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Complete));
        if (ai_request_backgrounded_) {
            state_.setStatus("AI patch ready. Press Alt+P to review.", 60);
        } else {
            state_.setActiveView(ViewKind::PatchPreview);
            state_.setStatus("AI patch ready for review.");
        }
        ai_request_backgrounded_ = false;
        return;
    }

    ShowAiText(response.raw_text, !ai_request_backgrounded_);
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Complete));
    state_.setStatus(ai_request_backgrounded_ ? "AI response complete. Press Alt+E to reopen AI scratch."
                                              : "AI response complete.",
                     60);
    ai_request_backgrounded_ = false;
}

void EditorApp::HandleAiError(const std::string& error_message) {
    if (active_ai_request_.has_value() && !active_ai_request_->streamed_text.empty()) {
        std::string combined = active_ai_request_->streamed_text;
        if (!combined.empty() && combined.back() != '\n') {
            combined += '\n';
        }
        combined += "\n[AI request failed]\n";
        combined += error_message;
        ShowAiText(combined, !ai_request_backgrounded_);
    } else {
        ShowAiText(error_message, !ai_request_backgrounded_);
    }
    state_.setAiRequestState(AiRequestStateLabel(AiRequestState::Failed));
    state_.setStatus(AiFailureStatus(error_message, ai_request_backgrounded_), 60);
    active_ai_request_.reset();
    ai_request_backgrounded_ = false;
}

void EditorApp::ShowAiText(const std::string& text, bool switch_to_ai_buffer) {
    state_.setAiText(text);
    if (switch_to_ai_buffer) {
        state_.setActiveView(ViewKind::AiScratch);
    }
}

void EditorApp::ReopenAiScratch() {
    state_.setActiveView(ViewKind::AiScratch);
    state_.setStatus("Opened AI scratch.");
}

void EditorApp::ReopenPatchPreview() {
    if (!state_.patchSession().has_value()) {
        state_.setStatus("No patch preview available.");
        return;
    }
    state_.setActiveView(ViewKind::PatchPreview);
    state_.setStatus("Opened patch preview.");
}

void EditorApp::InvalidatePatchSessionForManualFileEdit() {
    if (state_.patchSession().has_value()) {
        state_.setPatchSession(std::nullopt);
    }
}

void EditorApp::QuitEditor() {
    ai_client_->Shutdown();
    clangd_client_.Shutdown();
    active_ai_request_.reset();
    ai_request_backgrounded_ = false;
    state_.clearAiRequestState();
    state_.clearCompletionSession();
    running_ = false;
}

void EditorApp::HandlePatchAction(CommandType command_type) {
    if (!state_.patchSession().has_value()) {
        state_.setStatus("No patch session.");
        return;
    }

    PatchApplyResult result;
    switch (command_type) {
        case CommandType::PatchAccept:
            state_.BeginFileEdit();
            result = AcceptCurrentHunk(state_.fileBuffer(), *state_.patchSession());
            if (state_.CommitFileEdit()) {
                NotifyCompletionDocumentChanged();
            }
            break;
        case CommandType::PatchReject:
            RejectCurrentHunk(*state_.patchSession());
            result = {.success = true, .message = "Hunk rejected."};
            break;
        case CommandType::PatchAcceptAll:
            state_.BeginFileEdit();
            result = AcceptAllHunks(state_.fileBuffer(), *state_.patchSession());
            if (state_.CommitFileEdit()) {
                NotifyCompletionDocumentChanged();
            }
            break;
        case CommandType::PatchRejectAll:
            RejectAllHunks(*state_.patchSession());
            result = {.success = true, .message = "All pending hunks rejected."};
            break;
        default:
            return;
    }

    state_.syncPatchPreview();
    state_.setActiveView(ViewKind::PatchPreview);
    state_.setStatus(result.message.empty() ? "Patch action completed." : result.message);
}

}  // namespace flowstate
