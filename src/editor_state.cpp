#include "editor_state.h"

#include "cursor.h"
#include "selection.h"

namespace patchwork {

namespace {

bool SameCursor(const Cursor& left, const Cursor& right) {
    return left.row == right.row && left.col == right.col;
}

bool SameSelection(const Selection& left, const Selection& right) {
    return left.active == right.active && left.extend_on_cursor_move == right.extend_on_cursor_move &&
           SameCursor(left.anchor, right.anchor) && SameCursor(left.head, right.head);
}

}  // namespace

EditorState::EditorState(Buffer file_buffer)
    : file_buffer_(std::move(file_buffer)),
      ai_buffer_(BufferType::AiScratch, "AI Scratch", true),
      patch_buffer_(BufferType::PatchPreview, "Patch Preview", true),
      build_buffer_(BufferType::BuildOutput, "Build Output", true) {
    ai_buffer_.setText("AI responses will appear here.", false);
    ai_buffer_.clearDirty();
    patch_buffer_.setText("Patch previews will appear here.", false);
    patch_buffer_.clearDirty();
    build_buffer_.setText("Build output will appear here.", false);
    build_buffer_.clearDirty();
}

Buffer& EditorState::fileBuffer() { return file_buffer_; }

const Buffer& EditorState::fileBuffer() const { return file_buffer_; }

void EditorState::setFileBuffer(Buffer buffer) {
    file_buffer_ = std::move(buffer);
    file_view_ = {};
    selection_ = {};
    patch_session_.reset();
    patch_buffer_.setText("Patch previews will appear here.", false);
    patch_buffer_.clearDirty();
    active_view_ = ViewKind::File;
}

Buffer& EditorState::aiBuffer() { return ai_buffer_; }

Buffer& EditorState::patchBuffer() { return patch_buffer_; }

Buffer& EditorState::buildBuffer() { return build_buffer_; }

Buffer& EditorState::activeBuffer() { return bufferImpl(active_view_); }

const Buffer& EditorState::activeBuffer() const { return bufferImpl(active_view_); }

ViewKind EditorState::activeView() const { return active_view_; }

void EditorState::setActiveView(ViewKind view) {
    active_view_ = view;
    CursorController::clamp(activeViewport().cursor, activeBuffer());
}

Viewport& EditorState::viewport(ViewKind view) { return viewportImpl(view); }

const Viewport& EditorState::viewport(ViewKind view) const { return viewportImpl(view); }

Viewport& EditorState::activeViewport() { return viewportImpl(active_view_); }

const Viewport& EditorState::activeViewport() const { return viewportImpl(active_view_); }

Cursor& EditorState::fileCursor() { return file_view_.cursor; }

const Cursor& EditorState::fileCursor() const { return file_view_.cursor; }

Selection& EditorState::selection() { return selection_; }

const Selection& EditorState::selection() const { return selection_; }

void EditorState::clearSelection() { selection_ = {}; }

std::string EditorState::selectedOrCurrentText() const {
    const std::string selected = ExtractSelection(file_buffer_, selection_);
    if (!selected.empty()) {
        return selected;
    }
    return file_buffer_.currentLineText(file_view_.cursor.row);
}

void EditorState::setStatus(std::string text, int seconds) {
    status_text_ = std::move(text);
    status_expires_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

std::string EditorState::statusText() const {
    if (std::chrono::steady_clock::now() > status_expires_at_) {
        return {};
    }
    return status_text_;
}

void EditorState::setBuildCommand(std::string command) { build_command_ = std::move(command); }

const std::string& EditorState::buildCommand() const { return build_command_; }

void EditorState::setLastBuild(BuildResult result) { last_build_ = std::move(result); }

const std::optional<BuildResult>& EditorState::lastBuild() const { return last_build_; }

void EditorState::setAiText(const std::string& text) {
    ai_buffer_.setText(text, false);
    ai_buffer_.clearDirty();
    ai_view_.cursor.row = ai_buffer_.lineCount() > 0 ? ai_buffer_.lineCount() - 1 : 0;
    ai_view_.cursor.col = ai_buffer_.line(ai_view_.cursor.row).size();
}

void EditorState::setBuildOutput(const std::string& text) {
    build_buffer_.setText(text, false);
    build_buffer_.clearDirty();
}

void EditorState::setPatchSession(std::optional<PatchSession> session) {
    patch_session_ = std::move(session);
    syncPatchPreview();
}

std::optional<PatchSession>& EditorState::patchSession() { return patch_session_; }

const std::optional<PatchSession>& EditorState::patchSession() const { return patch_session_; }

void EditorState::syncPatchPreview() {
    if (!patch_session_.has_value()) {
        patch_buffer_.setText("Patch previews will appear here.", false);
        patch_buffer_.clearDirty();
        return;
    }

    patch_buffer_.setLines(RenderPatchPreview(*patch_session_), false);
    patch_buffer_.clearDirty();

    if (!patch_session_->preview_row_starts.empty()) {
        patch_session_->current_hunk =
            std::min(patch_session_->current_hunk, patch_session_->preview_row_starts.size() - 1);
        patch_view_.cursor.row = patch_session_->preview_row_starts[patch_session_->current_hunk];
        patch_view_.cursor.col = 0;
    }
}

void EditorState::setAiProviderName(std::string provider_name) {
    ai_provider_name_ = std::move(provider_name);
}

const std::string& EditorState::aiProviderName() const { return ai_provider_name_; }

void EditorState::setAiRequestState(std::string state) { ai_request_state_ = std::move(state); }

void EditorState::clearAiRequestState() { ai_request_state_.clear(); }

const std::string& EditorState::aiRequestState() const { return ai_request_state_; }

void EditorState::setClipboardText(std::string text) { clipboard_text_ = std::move(text); }

bool EditorState::hasClipboardText() const { return clipboard_text_.has_value(); }

std::string_view EditorState::clipboardText() const {
    if (!clipboard_text_.has_value()) {
        return {};
    }
    return *clipboard_text_;
}

void EditorState::clearClipboard() { clipboard_text_.reset(); }

void EditorState::BeginFileEdit() { pending_file_edit_ = CaptureFileHistoryEntry(); }

bool EditorState::CommitFileEdit() {
    if (!pending_file_edit_.has_value()) {
        return false;
    }

    const FileHistoryEntry current = CaptureFileHistoryEntry();
    if (SameFileHistoryEntry(*pending_file_edit_, current)) {
        pending_file_edit_.reset();
        return false;
    }

    undo_history_.push_back(*pending_file_edit_);
    redo_history_.clear();
    pending_file_edit_.reset();
    return true;
}

bool EditorState::UndoFileEdit() {
    pending_file_edit_.reset();
    if (undo_history_.empty()) {
        return false;
    }

    redo_history_.push_back(CaptureFileHistoryEntry());
    RestoreFileHistoryEntry(undo_history_.back());
    undo_history_.pop_back();
    return true;
}

bool EditorState::RedoFileEdit() {
    pending_file_edit_.reset();
    if (redo_history_.empty()) {
        return false;
    }

    undo_history_.push_back(CaptureFileHistoryEntry());
    RestoreFileHistoryEntry(redo_history_.back());
    redo_history_.pop_back();
    return true;
}

Viewport& EditorState::viewportImpl(ViewKind view) {
    switch (view) {
        case ViewKind::File:
            return file_view_;
        case ViewKind::AiScratch:
            return ai_view_;
        case ViewKind::PatchPreview:
            return patch_view_;
        case ViewKind::BuildOutput:
            return build_view_;
    }
    return file_view_;
}

const Viewport& EditorState::viewportImpl(ViewKind view) const {
    switch (view) {
        case ViewKind::File:
            return file_view_;
        case ViewKind::AiScratch:
            return ai_view_;
        case ViewKind::PatchPreview:
            return patch_view_;
        case ViewKind::BuildOutput:
            return build_view_;
    }
    return file_view_;
}

Buffer& EditorState::bufferImpl(ViewKind view) {
    switch (view) {
        case ViewKind::File:
            return file_buffer_;
        case ViewKind::AiScratch:
            return ai_buffer_;
        case ViewKind::PatchPreview:
            return patch_buffer_;
        case ViewKind::BuildOutput:
            return build_buffer_;
    }
    return file_buffer_;
}

const Buffer& EditorState::bufferImpl(ViewKind view) const {
    switch (view) {
        case ViewKind::File:
            return file_buffer_;
        case ViewKind::AiScratch:
            return ai_buffer_;
        case ViewKind::PatchPreview:
            return patch_buffer_;
        case ViewKind::BuildOutput:
            return build_buffer_;
    }
    return file_buffer_;
}

bool EditorState::SameFileHistoryEntry(const FileHistoryEntry& left, const FileHistoryEntry& right) {
    return left.lines == right.lines && SameCursor(left.cursor, right.cursor) &&
           SameSelection(left.selection, right.selection) && left.dirty == right.dirty;
}

EditorState::FileHistoryEntry EditorState::CaptureFileHistoryEntry() const {
    return FileHistoryEntry{
        .lines = file_buffer_.lines(),
        .cursor = file_view_.cursor,
        .selection = selection_,
        .dirty = file_buffer_.dirty(),
    };
}

void EditorState::RestoreFileHistoryEntry(const FileHistoryEntry& entry) {
    file_buffer_.setLines(entry.lines, false);
    file_buffer_.setDirty(entry.dirty);
    file_view_.cursor = entry.cursor;
    CursorController::clamp(file_view_.cursor, file_buffer_);
    selection_ = entry.selection;
}

}  // namespace patchwork
