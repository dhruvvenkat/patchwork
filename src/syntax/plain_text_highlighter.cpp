#include "syntax/plain_text_highlighter.h"

namespace patchwork {

LanguageId PlainTextHighlighter::language() const { return LanguageId::PlainText; }

SyntaxLineState PlainTextHighlighter::HighlightLine(std::string_view line,
                                                    SyntaxLineState in_state,
                                                    std::vector<SyntaxSpan>* spans) const {
    (void)line;
    (void)in_state;
    if (spans != nullptr) {
        spans->clear();
    }
    return {};
}

}  // namespace patchwork
