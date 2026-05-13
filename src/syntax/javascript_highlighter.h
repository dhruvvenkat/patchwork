#pragma once

#include "syntax/highlighter.h"

namespace flowstate {

class JavaScriptHighlighter : public ISyntaxHighlighter {
  public:
    explicit JavaScriptHighlighter(LanguageId language_id);

    LanguageId language() const override;
    SyntaxLineState HighlightLine(std::string_view line,
                                  SyntaxLineState in_state,
                                  std::vector<SyntaxSpan>* spans) const override;

  private:
    LanguageId language_id_;
};

}  // namespace flowstate
