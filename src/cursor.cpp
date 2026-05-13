#include "cursor.h"

#include <algorithm>

namespace flowstate {

void CursorController::clamp(Cursor& cursor, const Buffer& buffer) {
    if (buffer.lineCount() == 0) {
        cursor.row = 0;
        cursor.col = 0;
        return;
    }

    cursor.row = std::min(cursor.row, buffer.lineCount() - 1);
    cursor.col = std::min(cursor.col, buffer.line(cursor.row).size());
}

void CursorController::moveLeft(Cursor& cursor, const Buffer& buffer) {
    clamp(cursor, buffer);
    if (cursor.col > 0) {
        --cursor.col;
        return;
    }
    if (cursor.row > 0) {
        --cursor.row;
        cursor.col = buffer.line(cursor.row).size();
    }
}

void CursorController::moveRight(Cursor& cursor, const Buffer& buffer) {
    clamp(cursor, buffer);
    const size_t line_size = buffer.line(cursor.row).size();
    if (cursor.col < line_size) {
        ++cursor.col;
        return;
    }
    if (cursor.row + 1 < buffer.lineCount()) {
        ++cursor.row;
        cursor.col = 0;
    }
}

void CursorController::moveUp(Cursor& cursor, const Buffer& buffer) {
    clamp(cursor, buffer);
    if (cursor.row > 0) {
        --cursor.row;
        cursor.col = std::min(cursor.col, buffer.line(cursor.row).size());
    }
}

void CursorController::moveDown(Cursor& cursor, const Buffer& buffer) {
    clamp(cursor, buffer);
    if (cursor.row + 1 < buffer.lineCount()) {
        ++cursor.row;
        cursor.col = std::min(cursor.col, buffer.line(cursor.row).size());
    }
}

void CursorController::moveHome(Cursor& cursor) { cursor.col = 0; }

void CursorController::moveEnd(Cursor& cursor, const Buffer& buffer) {
    clamp(cursor, buffer);
    cursor.col = buffer.line(cursor.row).size();
}

}  // namespace flowstate

