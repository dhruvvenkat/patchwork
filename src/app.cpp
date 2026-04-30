#include "app.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

#include "ai/mock_client.h"
#include "command.h"
#include "cursor.h"
#include "diff.h"
#include "patch.h"
#include "selection.h"

namespace patchwork {

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

}  // namespace

EditorApp::EditorApp(Buffer file_buffer,
                     std::unique_ptr<IAiClient> ai_client,
                     std::string build_command,
                     std::string ai_provider_name)
    : state_(std::move(file_buffer)), ai_client_(std::move(ai_client)) {
    state_.setBuildCommand(std::move(build_command));
    state_.setAiProviderName(std::move(ai_provider_name));
    state_.setStatus("Ctrl+G selects, Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes.");
}

int EditorApp::Run() {
    std::string terminal_error;
    if (!terminal_.EnableRawMode(&terminal_error)) {
        terminal_.Write("Failed to initialize terminal raw mode: " + terminal_error + "\n");
        return 1;
    }

    while (running_) {
        PollAiRequest();
        RefreshScreen();
        const KeyPress key = terminal_.ReadKey();
        PollAiRequest();
        if (key.type == KeyType::Unknown) {
            continue;
        }
        if (command_mode_) {
            HandleCommandKey(key);
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
    terminal_.Write(screen_.Render(state_, {.command_mode = command_mode_, .command_input = command_input_},
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
        if (key.ch == 'e') {
            ReopenAiScratch();
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
            MoveCursor(key.type);
            return;
        case KeyType::Home:
            CursorController::moveHome(state_.activeViewport().cursor);
            UpdateSelectionHead();
            return;
        case KeyType::End:
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
                state_.fileBuffer().deleteCharBefore(state_.fileCursor());
            }
            return;
        case KeyType::DeleteKey:
            if (state_.activeView() == ViewKind::File) {
                state_.fileBuffer().deleteCharAt(state_.fileCursor());
            }
            return;
        case KeyType::Enter:
            if (state_.activeView() == ViewKind::File) {
                state_.fileBuffer().insertNewline(state_.fileCursor());
                UpdateSelectionHead();
            }
            return;
        case KeyType::Character:
            if (key.ch == ':') {
                command_mode_ = true;
                command_input_.clear();
                return;
            }
            if (state_.activeView() == ViewKind::File && !key.ctrl &&
                static_cast<unsigned char>(key.ch) >= 32) {
                state_.fileBuffer().insertChar(state_.fileCursor(), key.ch);
                ++state_.fileCursor().col;
                UpdateSelectionHead();
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

void EditorApp::UpdateSelectionHead() {
    if (state_.activeView() == ViewKind::File && state_.selection().active) {
        state_.selection().head = state_.fileCursor();
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
    state_.setStatus("Opened " + path + ".");
    return true;
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
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
        state_.fileBuffer().deleteRange(cursor, range.start, range.end);
        state_.clearSelection();
        state_.setStatus("Cut selection.");
        return;
    }

    const SelectionRange range = CurrentLineRange(state_.fileBuffer(), cursor);
    state_.setClipboardText(ExtractRange(state_.fileBuffer(), range));
    state_.fileBuffer().deleteRange(cursor, range.start, range.end);
    cursor.col = 0;
    CursorController::clamp(cursor, state_.fileBuffer());
    state_.clearSelection();
    state_.setStatus("Cut line.");
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
    if (HasSelection(state_.selection())) {
        const SelectionRange range = NormalizeSelection(state_.selection());
        state_.fileBuffer().replaceRange(cursor, range.start, range.end, state_.clipboardText());
    } else {
        state_.fileBuffer().insertText(cursor, state_.clipboardText());
    }

    state_.clearSelection();
    CursorController::clamp(cursor, state_.fileBuffer());
    state_.setStatus("Pasted clipboard.");
}

void EditorApp::RunBuild() {
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

void EditorApp::QuitEditor() {
    ai_client_->Shutdown();
    active_ai_request_.reset();
    ai_request_backgrounded_ = false;
    state_.clearAiRequestState();
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
            result = AcceptCurrentHunk(state_.fileBuffer(), *state_.patchSession());
            break;
        case CommandType::PatchReject:
            RejectCurrentHunk(*state_.patchSession());
            result = {.success = true, .message = "Hunk rejected."};
            break;
        case CommandType::PatchAcceptAll:
            result = AcceptAllHunks(state_.fileBuffer(), *state_.patchSession());
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

}  // namespace patchwork
