#pragma once

#include <string_view>

#include "syntax/highlighter.h"

namespace flowstate {

std::string_view ColorCodeForToken(SyntaxTokenKind token_kind);
std::string_view ResetColorCode();

}  // namespace flowstate
