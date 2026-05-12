#pragma once

#include "syntax/highlighter.h"

namespace flowstate {

const ISyntaxHighlighter& HighlighterForLanguage(LanguageId language_id);

}  // namespace flowstate
