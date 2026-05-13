#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "syntax/language.h"

namespace flowstate {

enum class SyntaxTokenKind {
    Default,
    Comment,
    Preprocessor,
    IncludePath,
    Macro,
    Keyword,
    Type,
    String,
    Number,
    Function,
    Heading,
    Quote,
    ListMarker,
    CodeFence,
    InlineCode,
    LinkText,
    LinkUrl,
    Emphasis,
};

struct SyntaxSpan {
    size_t start = 0;
    size_t end = 0;
    SyntaxTokenKind kind = SyntaxTokenKind::Default;
};

struct SyntaxLineState {
    uint64_t value = 0;

    bool operator==(const SyntaxLineState& other) const = default;
};

class ISyntaxHighlighter {
  public:
    virtual ~ISyntaxHighlighter() = default;

    virtual LanguageId language() const = 0;
    virtual SyntaxLineState InitialState() const { return {}; }
    virtual SyntaxLineState HighlightLine(std::string_view line,
                                          SyntaxLineState in_state,
                                          std::vector<SyntaxSpan>* spans) const = 0;
};

}  // namespace flowstate
