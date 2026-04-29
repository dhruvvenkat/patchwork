#pragma once

#include <memory>
#include <string>

#include "ai/client.h"
#include "command.h"
#include "editor_state.h"
#include "screen.h"
#include "terminal.h"

namespace patchwork {

class EditorApp {
  public:
    EditorApp(Buffer file_buffer, std::unique_ptr<IAiClient> ai_client, std::string build_command);

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
    void ShowAiText(const std::string& text);
    void HandlePatchAction(CommandType command_type);

    Terminal terminal_;
    Screen screen_;
    EditorState state_;
    std::unique_ptr<IAiClient> ai_client_;
    bool running_ = true;
    bool command_mode_ = false;
    std::string command_input_;
    bool pending_quit_confirm_ = false;
};

}  // namespace patchwork
