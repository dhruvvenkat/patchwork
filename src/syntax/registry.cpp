#include "syntax/registry.h"

#include "syntax/cpp_highlighter.h"
#include "syntax/javascript_highlighter.h"
#include "syntax/plain_text_highlighter.h"
#include "syntax/python_highlighter.h"
#include "syntax/rust_highlighter.h"

namespace patchwork {

const ISyntaxHighlighter& HighlighterForLanguage(LanguageId language_id) {
    static const PlainTextHighlighter kPlainTextHighlighter;
    static const CppHighlighter kCppHighlighter;
    static const JavaScriptHighlighter kJavaScriptHighlighter(LanguageId::JavaScript);
    static const JavaScriptHighlighter kTypeScriptHighlighter(LanguageId::TypeScript);
    static const PythonHighlighter kPythonHighlighter;
    static const RustHighlighter kRustHighlighter;

    switch (language_id) {
        case LanguageId::C:
        case LanguageId::CHeader:
        case LanguageId::Cpp:
            return kCppHighlighter;
        case LanguageId::Rust:
            return kRustHighlighter;
        case LanguageId::Python:
            return kPythonHighlighter;
        case LanguageId::JavaScript:
            return kJavaScriptHighlighter;
        case LanguageId::TypeScript:
            return kTypeScriptHighlighter;
        case LanguageId::PlainText:
        case LanguageId::Java:
        case LanguageId::Go:
        case LanguageId::Markdown:
            return kPlainTextHighlighter;
    }

    return kPlainTextHighlighter;
}

}  // namespace patchwork
