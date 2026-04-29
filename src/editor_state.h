#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "build.h"
#include "buffer.h"
#include "cursor.h"
#include "patch.h"
#include "selection.h"

namespace patchwork {

enum class ViewKind {
    File,
    AiScratch,
    PatchPreview,
    BuildOutput,
};

class EditorState {
  public:
    explicit EditorState(Buffer file_buffer);

    Buffer& fileBuffer();
    const Buffer& fileBuffer() const;
    void setFileBuffer(Buffer buffer);

    Buffer& aiBuffer();
    Buffer& patchBuffer();
    Buffer& buildBuffer();
    Buffer& activeBuffer();
    const Buffer& activeBuffer() const;

    ViewKind activeView() const;
    void setActiveView(ViewKind view);

    Viewport& viewport(ViewKind view);
    const Viewport& viewport(ViewKind view) const;
    Viewport& activeViewport();
    const Viewport& activeViewport() const;

    Cursor& fileCursor();
    const Cursor& fileCursor() const;

    Selection& selection();
    const Selection& selection() const;
    void clearSelection();
    std::string selectedOrCurrentText() const;

    void setStatus(std::string text, int seconds = 5);
    std::string statusText() const;

    void setBuildCommand(std::string command);
    const std::string& buildCommand() const;
    void setLastBuild(BuildResult result);
    const std::optional<BuildResult>& lastBuild() const;

    void setAiText(const std::string& text);
    void setBuildOutput(const std::string& text);

    void setPatchSession(std::optional<PatchSession> session);
    std::optional<PatchSession>& patchSession();
    const std::optional<PatchSession>& patchSession() const;
    void syncPatchPreview();

    void setAiProviderName(std::string provider_name);
    const std::string& aiProviderName() const;

  private:
    Viewport& viewportImpl(ViewKind view);
    const Viewport& viewportImpl(ViewKind view) const;
    Buffer& bufferImpl(ViewKind view);
    const Buffer& bufferImpl(ViewKind view) const;

    Buffer file_buffer_;
    Buffer ai_buffer_;
    Buffer patch_buffer_;
    Buffer build_buffer_;

    ViewKind active_view_ = ViewKind::File;
    Viewport file_view_;
    Viewport ai_view_;
    Viewport patch_view_;
    Viewport build_view_;

    Selection selection_;
    std::string status_text_;
    std::string ai_provider_name_ = "MOCK";
    std::chrono::steady_clock::time_point status_expires_at_{};
    std::string build_command_;
    std::optional<BuildResult> last_build_;
    std::optional<PatchSession> patch_session_;
};

}  // namespace patchwork
