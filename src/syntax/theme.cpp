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
        case SyntaxTokenKind::Heading:
            return "\x1b[38;5;111m";
        case SyntaxTokenKind::Quote:
            return "\x1b[38;5;66m";
        case SyntaxTokenKind::ListMarker:
            return "\x1b[38;5;215m";
        case SyntaxTokenKind::CodeFence:
            return "\x1b[38;5;141m";
        case SyntaxTokenKind::InlineCode:
            return "\x1b[38;5;220m";
        case SyntaxTokenKind::LinkText:
            return "\x1b[38;5;117m";
        case SyntaxTokenKind::LinkUrl:
            return "\x1b[38;5;214m";
        case SyntaxTokenKind::Emphasis:
            return "\x1b[38;5;211m";
        case SyntaxTokenKind::Default:
            return "\x1b[39m";
    }

    return "\x1b[39m";
}

std::string_view ResetColorCode() { return "\x1b[39m"; }

}  // namespace patchwork
