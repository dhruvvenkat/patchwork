#include "syntax/theme.h"

namespace patchwork {

std::string_view ColorCodeForToken(SyntaxTokenKind token_kind) {
    switch (token_kind) {
        case SyntaxTokenKind::Comment:
            return "\x1b[38;5;30m";
        case SyntaxTokenKind::Preprocessor:
            return "\x1b[38;5;141m";
        case SyntaxTokenKind::IncludePath:
            return "\x1b[38;5;214m";
        case SyntaxTokenKind::Macro:
            return "\x1b[38;5;220m";
        case SyntaxTokenKind::Keyword:
            return "\x1b[38;5;75m";
        case SyntaxTokenKind::Type:
            return "\x1b[38;5;81m";
        case SyntaxTokenKind::String:
            return "\x1b[38;5;221m";
        case SyntaxTokenKind::Number:
            return "\x1b[38;5;179m";
        case SyntaxTokenKind::Function:
            return "\x1b[38;5;117m";
        case SyntaxTokenKind::Default:
            return "\x1b[39m";
    }

    return "\x1b[39m";
}

std::string_view ResetColorCode() { return "\x1b[39m"; }

}  // namespace patchwork
