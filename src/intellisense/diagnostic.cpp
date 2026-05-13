#include "intellisense/diagnostic.h"

namespace flowstate {

bool HasErrorDiagnosticAt(const std::vector<Diagnostic>& diagnostics, size_t row, size_t col) {
    return ErrorDiagnosticAt(diagnostics, row, col) != nullptr;
}

const Diagnostic* ErrorDiagnosticAt(const std::vector<Diagnostic>& diagnostics, size_t row, size_t col) {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) {
            continue;
        }
        const Cursor& start = diagnostic.range.start;
        const Cursor& end = diagnostic.range.end;
        if (row < start.row || row > end.row) {
            continue;
        }
        if (start.row == end.row) {
            if (start.col == end.col) {
                const size_t underline_col = start.col == 0 ? 0 : start.col - 1;
                if (row == start.row && col == underline_col) {
                    return &diagnostic;
                }
                continue;
            }
            if (row == start.row && col >= start.col && col < end.col) {
                return &diagnostic;
            }
            continue;
        }
        if (row == start.row && col >= start.col) {
            return &diagnostic;
        }
        if (row == end.row && col < end.col) {
            return &diagnostic;
        }
        if (row > start.row && row < end.row) {
            return &diagnostic;
        }
    }
    return nullptr;
}

}  // namespace flowstate
