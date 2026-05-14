#pragma once

#include <string>
#include "editor_state.h"

namespace flowstate {

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
    size_t InlineAiBodyRowCount(const EditorState& state, size_t content_cols) const;
    size_t InlineAiVisibleBodyRowCount(const EditorState& state, size_t content_cols) const;
    size_t InlineAiRowCount(const EditorState& state, size_t content_cols) const;
    size_t InlineAiRowsBetween(const EditorState& state,
                               size_t content_cols,
                               size_t start_row,
                               size_t end_row) const;
    std::string Render(const EditorState& state, const RenderOptions& options, int rows, int cols) const;
};

}  // namespace flowstate
