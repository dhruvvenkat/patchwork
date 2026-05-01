#pragma once

#include <string>
#include "editor_state.h"

namespace patchwork {

struct RenderOptions {
    bool command_mode = false;
    std::string command_input;
    bool file_picker_mode = false;
    std::string file_picker_query;
    size_t file_picker_selected = 0;
};

class Screen {
  public:
    size_t ContentColumns(const EditorState& state, int total_cols) const;
    std::string Render(const EditorState& state, const RenderOptions& options, int rows, int cols) const;
};

}  // namespace patchwork
