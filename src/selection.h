#pragma once

#include <string>

#include "buffer.h"

namespace flowstate {

struct Selection {
    bool active = false;
    bool extend_on_cursor_move = false;
    Cursor anchor;
    Cursor head;
};

struct SelectionRange {
    Cursor start;
    Cursor end;
};

bool HasSelection(const Selection& selection);
SelectionRange NormalizeSelection(const Selection& selection);
std::string ExtractRange(const Buffer& buffer, const SelectionRange& range);
std::string ExtractSelection(const Buffer& buffer, const Selection& selection);
SelectionRange CurrentLineRange(const Buffer& buffer, const Cursor& cursor);
bool IsPositionSelected(const Selection& selection, size_t row, size_t col);

}  // namespace flowstate
