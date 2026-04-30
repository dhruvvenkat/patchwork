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

Cursor ClampCursorToBuffer(const Buffer& buffer, Cursor cursor) {
    if (buffer.lineCount() == 0) {
        return {};
    }

    cursor.row = std::min(cursor.row, buffer.lineCount() - 1);
    cursor.col = std::min(cursor.col, buffer.line(cursor.row).size());
    return cursor;
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

std::string ExtractRange(const Buffer& buffer, const SelectionRange& range) {
    if (buffer.lineCount() == 0) {
        return {};
    }

    SelectionRange normalized = range;
    normalized.start = ClampCursorToBuffer(buffer, normalized.start);
    normalized.end = ClampCursorToBuffer(buffer, normalized.end);
    if (!ComesBefore(normalized.start, normalized.end)) {
        std::swap(normalized.start, normalized.end);
    }
    if (normalized.start.row == normalized.end.row && normalized.start.col == normalized.end.col) {
        return {};
    }

    std::ostringstream output;

    for (size_t row = normalized.start.row; row <= normalized.end.row && row < buffer.lineCount(); ++row) {
        const std::string& line = buffer.line(row);
        const size_t start_col = (row == normalized.start.row) ? std::min(normalized.start.col, line.size()) : 0;
        const size_t end_col =
            (row == normalized.end.row) ? std::min(normalized.end.col, line.size()) : line.size();

        if (row == normalized.start.row && row == normalized.end.row) {
            output << line.substr(start_col, end_col - start_col);
        } else if (row == normalized.start.row) {
            output << line.substr(start_col);
        } else if (row == normalized.end.row) {
            output << line.substr(0, end_col);
        } else {
            output << line;
        }

        if (row != normalized.end.row) {
            output << '\n';
        }
    }

    return output.str();
}

std::string ExtractSelection(const Buffer& buffer, const Selection& selection) {
    if (!HasSelection(selection)) {
        return {};
    }
    return ExtractRange(buffer, NormalizeSelection(selection));
}

SelectionRange CurrentLineRange(const Buffer& buffer, const Cursor& cursor) {
    if (buffer.lineCount() == 0) {
        return {};
    }

    const size_t row = std::min(cursor.row, buffer.lineCount() - 1);
    if (row + 1 < buffer.lineCount()) {
        return {{row, 0}, {row + 1, 0}};
    }
    return {{row, 0}, {row, buffer.line(row).size()}};
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
