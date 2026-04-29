#pragma once

#include <chrono>
#include <future>
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
    void RunBuild();
    void RunAiRequest(AiRequestKind kind, std::string instruction);
    AiRequest BuildAiRequest(AiRequestKind kind, const std::string& instruction) const;
    void PollAiRequest();
    void UpdateAiLoadingView();
    void PollAiReveal();
    void HandleAiResponse(const AiResponse& response);
    void ShowAiText(const std::string& text);
    void HandlePatchAction(CommandType command_type);

    struct PendingAiRequest {
        std::future<AiResponse> future;
        std::string label;
    };

    struct PendingAiReveal {
        AiResponse response;
        std::string display_text;
        size_t visible_bytes = 0;
        std::chrono::steady_clock::time_point next_step_at{};
    };

    Terminal terminal_;
    Screen screen_;
    EditorState state_;
    std::unique_ptr<IAiClient> ai_client_;
    bool running_ = true;
    bool command_mode_ = false;
    std::string command_input_;
    bool pending_quit_confirm_ = false;
    std::optional<PendingAiRequest> pending_ai_request_;
    std::optional<PendingAiReveal> pending_ai_reveal_;
    std::string ai_loading_label_;
    std::chrono::steady_clock::time_point next_ai_loading_tick_{};
    size_t ai_loading_frame_ = 0;
};

}  // namespace patchwork
