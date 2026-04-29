#include "syntax/registry.h"

#include "syntax/cpp_highlighter.h"
#include "syntax/plain_text_highlighter.h"

namespace patchwork {

const ISyntaxHighlighter& HighlighterForLanguage(LanguageId language_id) {
    static const PlainTextHighlighter kPlainTextHighlighter;
    static const CppHighlighter kCppHighlighter;

    switch (language_id) {
        case LanguageId::C:
        case LanguageId::CHeader:
        case LanguageId::Cpp:
            return kCppHighlighter;
        case LanguageId::PlainText:
        case LanguageId::Rust:
        case LanguageId::Python:
        case LanguageId::JavaScript:
        case LanguageId::TypeScript:
        case LanguageId::Java:
        case LanguageId::Go:
        case LanguageId::Markdown:
            return kPlainTextHighlighter;
    }

    return kPlainTextHighlighter;
}

}  // namespace patchwork
