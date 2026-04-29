#include "selection.h"

#include <algorithm>
#include <sstream>

namespace patchwork {

namespace {

bool ComesBefore(const Cursor& left, const Cursor& right) {
    if (left.row != right.row) {
        return left.row < right.row;
    }
    return left.col <= right.col;
}

}  // namespace

bool HasSelection(const Selection& selection) {
    return selection.active &&
           (selection.anchor.row != selection.head.row || selection.anchor.col != selection.head.col);
}

SelectionRange NormalizeSelection(const Selection& selection) {
    if (ComesBefore(selection.anchor, selection.head)) {
        return {selection.anchor, selection.head};
    }
    return {selection.head, selection.anchor};
}

std::string ExtractSelection(const Buffer& buffer, const Selection& selection) {
    if (!HasSelection(selection)) {
        return {};
    }

    const SelectionRange range = NormalizeSelection(selection);
    std::ostringstream output;

    for (size_t row = range.start.row; row <= range.end.row && row < buffer.lineCount(); ++row) {
        const std::string& line = buffer.line(row);
        const size_t start_col = (row == range.start.row) ? std::min(range.start.col, line.size()) : 0;
        const size_t end_col =
            (row == range.end.row) ? std::min(range.end.col, line.size()) : line.size();

        if (row == range.start.row && row == range.end.row) {
            output << line.substr(start_col, end_col - start_col);
        } else if (row == range.start.row) {
            output << line.substr(start_col);
        } else if (row == range.end.row) {
            output << line.substr(0, end_col);
        } else {
            output << line;
        }

        if (row != range.end.row) {
            output << '\n';
        }
    }

    return output.str();
}

bool IsPositionSelected(const Selection& selection, size_t row, size_t col) {
    if (!HasSelection(selection)) {
        return false;
    }

    const SelectionRange range = NormalizeSelection(selection);
    const Cursor position{row, col};
    return ComesBefore(range.start, position) && ComesBefore(position, range.end);
}

}  // namespace patchwork

