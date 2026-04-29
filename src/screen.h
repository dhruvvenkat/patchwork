#pragma once

#include <string>

#include "editor_state.h"

namespace patchwork {

struct RenderOptions {
    bool command_mode = false;
    std::string command_input;
};

class Screen {
  public:
    std::string Render(const EditorState& state, const RenderOptions& options, int rows, int cols) const;
};

}  // namespace patchwork

