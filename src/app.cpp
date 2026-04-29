#include "app.h"

#include <algorithm>
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

}  // namespace

EditorApp::EditorApp(Buffer file_buffer,
                     std::unique_ptr<IAiClient> ai_client,
                     std::string build_command,
                     std::string ai_provider_name)
    : state_(std::move(file_buffer)), ai_client_(std::move(ai_client)) {
    state_.setBuildCommand(std::move(build_command));
    state_.setAiProviderName(std::move(ai_provider_name));
    state_.setStatus("Ctrl+G selects, Ctrl+E explains, Ctrl+R requests a patch.");
}

int EditorApp::Run() {
    std::string terminal_error;
    if (!terminal_.EnableRawMode(&terminal_error)) {
        terminal_.Write("Failed to initialize terminal raw mode: " + terminal_error + "\n");
        return 1;
    }

    while (running_) {
        RefreshScreen();
        const KeyPress key = terminal_.ReadKey();
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

    if (viewport.cursor.row < viewport.row_offset) {
        viewport.row_offset = viewport.cursor.row;
    }
    if (viewport.cursor.row >= viewport.row_offset + static_cast<size_t>(content_rows)) {
        viewport.row_offset = viewport.cursor.row - static_cast<size_t>(content_rows) + 1;
    }
    if (viewport.cursor.col < viewport.col_offset) {
        viewport.col_offset = viewport.cursor.col;
    }
    if (viewport.cursor.col >= viewport.col_offset + static_cast<size_t>(screen_cols)) {
        viewport.col_offset = viewport.cursor.col - static_cast<size_t>(screen_cols) + 1;
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
                    running_ = false;
                }
                return;
            case 's':
                SaveFile();
                return;
            case 'e':
                RunAiRequest(AiRequestKind::Explain, "Explain this code.");
                return;
            case 'r':
                RunAiRequest(AiRequestKind::Fix, "Fix bugs in this code and keep the patch minimal.");
                return;
            case 'b':
                RunBuild();
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
                state_.setStatus("Returned to file buffer.");
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
                running_ = false;
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
    if (kind == AiRequestKind::ErrorExplain && !state_.lastBuild().has_value()) {
        state_.setStatus("Run :build before :ai error.");
        return;
    }

    const AiRequest request = BuildAiRequest(kind, instruction);
    const AiResponse response = ai_client_->Complete(request);
    if (response.kind == AiResponseKind::Error) {
        ShowAiText(response.error_message);
        return;
    }

    if (response.diff_text.has_value()) {
        const PatchSet patch = ParseUnifiedDiff(*response.diff_text);
        if (!patch.valid()) {
            ShowAiText(response.raw_text);
            state_.setStatus("AI returned a malformed patch.");
            return;
        }
        if (!PatchTargetsBuffer(patch, state_.fileBuffer())) {
            ShowAiText(response.raw_text);
            state_.setStatus("AI patch does not target the current file.");
            return;
        }

        state_.setAiText(response.raw_text);
        state_.setPatchSession(CreatePatchSession(patch, state_.fileBuffer()));
        state_.setActiveView(ViewKind::PatchPreview);
        state_.setStatus("AI patch ready for review.");
        return;
    }

    ShowAiText(response.raw_text);
}

void EditorApp::ShowAiText(const std::string& text) {
    state_.setAiText(text);
    state_.setActiveView(ViewKind::AiScratch);
    state_.setStatus("AI response loaded.");
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
