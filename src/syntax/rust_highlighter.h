#pragma once

#include "syntax/highlighter.h"

namespace flowstate {

class RustHighlighter : public ISyntaxHighlighter {
  public:
    LanguageId language() const override;
    SyntaxLineState HighlightLine(std::string_view line,
                                  SyntaxLineState in_state,
                                  std::vector<SyntaxSpan>* spans) const override;
};

}  // namespace flowstate
