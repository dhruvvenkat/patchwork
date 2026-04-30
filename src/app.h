#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "ai/client.h"
#include "command.h"
#include "editor_state.h"
#include "screen.h"
#include "terminal.h"

namespace patchwork {

class EditorApp {
  public:
    EditorApp(Buffer file_buffer,
              std::unique_ptr<IAiClient> ai_client,
              std::string build_command,
              std::string ai_provider_name);

    int Run();

  private:
    void RefreshScreen();
    void ScrollToCursor(int screen_rows, int screen_cols);
    void HandleNormalKey(const KeyPress& key);
    void HandleCommandKey(const KeyPress& key);
    void MoveCursor(KeyType key, size_t distance = 1);
    void UpdateSelectionHead();
    bool ExecuteCommand(const Command& command);
    bool OpenFile(const std::string& path);
    void SaveFile();
    void ToggleSelection();
    void CopySelectionOrLine();
    void CutSelectionOrLine();
    void PasteClipboard();
    void RunBuild();
    void RunAiRequest(AiRequestKind kind, std::string instruction);
    AiRequest BuildAiRequest(AiRequestKind kind, const std::string& instruction) const;
    void PollAiRequest();
    void UpdateAiLoadingView();
    void RenderActiveAiScratch();
    void HandleAiResponse(const AiResponse& response);
    void HandleAiError(const std::string& error_message);
    void ShowAiText(const std::string& text, bool switch_to_ai_buffer);
    void ReopenAiScratch();
    void ReopenPatchPreview();
    void QuitEditor();
    void HandlePatchAction(CommandType command_type);

    struct ActiveAiRequest {
        AiRequestKind kind = AiRequestKind::Explain;
        std::string label;
        std::string streamed_text;
    };

    Terminal terminal_;
    Screen screen_;
    EditorState state_;
    std::unique_ptr<IAiClient> ai_client_;
    bool running_ = true;
    bool command_mode_ = false;
    std::string command_input_;
    bool pending_quit_confirm_ = false;
    std::optional<ActiveAiRequest> active_ai_request_;
    bool ai_request_backgrounded_ = false;
    std::chrono::steady_clock::time_point next_ai_loading_tick_{};
    size_t ai_loading_frame_ = 0;
};

}  // namespace patchwork
