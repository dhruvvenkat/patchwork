#pragma once

#include <string>

#include "buffer.h"

namespace patchwork {

struct Selection {
    bool active = false;
    Cursor anchor;
    Cursor head;
};

struct SelectionRange {
    Cursor start;
    Cursor end;
};

bool HasSelection(const Selection& selection);
SelectionRange NormalizeSelection(const Selection& selection);
std::string ExtractSelection(const Buffer& buffer, const Selection& selection);
bool IsPositionSelected(const Selection& selection, size_t row, size_t col);

}  // namespace patchwork

