#pragma once

#include "syntax/highlighter.h"

namespace patchwork {

const ISyntaxHighlighter& HighlighterForLanguage(LanguageId language_id);

}  // namespace patchwork
