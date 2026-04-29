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
        case SyntaxTokenKind::Default:
            return "\x1b[39m";
    }

    return "\x1b[39m";
}

std::string_view ResetColorCode() { return "\x1b[39m"; }

}  // namespace patchwork
