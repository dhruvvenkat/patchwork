#pragma once

#include "buffer.h"

namespace patchwork {

struct Viewport {
    Cursor cursor;
    size_t row_offset = 0;
    size_t col_offset = 0;
};

class CursorController {
  public:
    static void clamp(Cursor& cursor, const Buffer& buffer);
    static void moveLeft(Cursor& cursor, const Buffer& buffer);
    static void moveRight(Cursor& cursor, const Buffer& buffer);
    static void moveUp(Cursor& cursor, const Buffer& buffer);
    static void moveDown(Cursor& cursor, const Buffer& buffer);
    static void moveHome(Cursor& cursor);
    static void moveEnd(Cursor& cursor, const Buffer& buffer);
};

}  // namespace patchwork

