#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "ai/codex_client.h"
#include "ai/mock_client.h"
#include "build.h"
#include "buffer.h"
#include "command.h"
#include "diff.h"
#include "editor_state.h"
#include "json.h"
#include "patch.h"
#include "selection.h"
#include "screen.h"
#include "syntax/cpp_highlighter.h"
#include "syntax/go_highlighter.h"
#include "syntax/javascript_highlighter.h"
#include "syntax/java_highlighter.h"
#include "syntax/markdown_highlighter.h"
#include "syntax/python_highlighter.h"
#include "syntax/rust_highlighter.h"

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestBufferEditing() {
    patchwork::Buffer buffer;
    patchwork::Cursor cursor;
    buffer.insertChar(cursor, 'a');
    cursor.col = 1;
    buffer.insertChar(cursor, 'b');
    cursor.col = 2;
    buffer.insertNewline(cursor);
    buffer.insertChar(cursor, 'c');

    Expect(buffer.lineCount() == 2, "buffer should have two lines");
    Expect(buffer.line(0) == "ab", "first line should be preserved");
    Expect(buffer.line(1) == "c", "second line should contain inserted text");

    buffer.deleteCharBefore(cursor);
    Expect(buffer.line(1).empty(), "backspace should remove the character");
}

void TestSelectionExtraction() {
    patchwork::Buffer buffer;
    buffer.setText("alpha\nbeta\ngamma", false);
    patchwork::Selection selection{
        .active = true,
        .anchor = {0, 2},
        .head = {2, 2},
    };

    const std::string extracted = patchwork::ExtractSelection(buffer, selection);
    Expect(extracted == "pha\nbeta\nga", "selection extraction should preserve multi-line ranges");
}

void TestSelectionRangeHelpers() {
    patchwork::Buffer buffer;
    buffer.setText("alpha\nbeta\ngamma", false);

    const patchwork::SelectionRange middle_line = patchwork::CurrentLineRange(buffer, {.row = 1, .col = 2});
    Expect(patchwork::ExtractRange(buffer, middle_line) == "beta\n",
           "current line ranges should include the trailing newline for non-final lines");

    const patchwork::SelectionRange final_line = patchwork::CurrentLineRange(buffer, {.row = 2, .col = 1});
    Expect(patchwork::ExtractRange(buffer, final_line) == "gamma",
           "current line ranges should not invent a trailing newline for the final line");
}

void TestBufferRangeEditing() {
    patchwork::Buffer insert_buffer;
    insert_buffer.setText("abcd", false);
    patchwork::Cursor insert_cursor{0, 2};
    insert_buffer.insertText(insert_cursor, "X\nY");
    Expect(insert_buffer.lineCount() == 2, "multi-line paste should split the current line");
    Expect(insert_buffer.line(0) == "abX", "paste should keep the line prefix before the cursor");
    Expect(insert_buffer.line(1) == "Ycd", "paste should keep the line suffix after the cursor");
    Expect(insert_cursor.row == 1 && insert_cursor.col == 1,
           "paste cursor should land at the end of the inserted text");

    patchwork::Buffer same_line_delete_buffer;
    same_line_delete_buffer.setText("abcd", false);
    patchwork::Cursor same_line_cursor{0, 0};
    same_line_delete_buffer.deleteRange(same_line_cursor, {.row = 0, .col = 1}, {.row = 0, .col = 3});
    Expect(same_line_delete_buffer.text() == "ad", "single-line range deletion should remove the selected text");
    Expect(same_line_cursor.row == 0 && same_line_cursor.col == 1,
           "single-line range deletion should move the cursor to the range start");

    patchwork::Buffer multi_line_delete_buffer;
    multi_line_delete_buffer.setText("alpha\nbeta\ngamma", false);
    patchwork::Cursor multi_line_cursor{0, 0};
    multi_line_delete_buffer.deleteRange(multi_line_cursor, {.row = 0, .col = 2}, {.row = 2, .col = 2});
    Expect(multi_line_delete_buffer.text() == "almma",
           "multi-line range deletion should join the prefix and suffix across lines");
    Expect(multi_line_cursor.row == 0 && multi_line_cursor.col == 2,
           "multi-line range deletion should move the cursor to the range start");

    patchwork::Buffer replace_buffer;
    replace_buffer.setText("abcde", false);
    patchwork::Cursor replace_cursor{0, 0};
    replace_buffer.replaceRange(replace_cursor, {.row = 0, .col = 1}, {.row = 0, .col = 4}, "X\nY");
    Expect(replace_buffer.lineCount() == 2, "replacing with multi-line text should split the buffer");
    Expect(replace_buffer.line(0) == "aX", "replace should preserve the prefix before the replaced range");
    Expect(replace_buffer.line(1) == "Ye", "replace should preserve the suffix after the replaced range");
    Expect(replace_cursor.row == 1 && replace_cursor.col == 1,
           "replace should leave the cursor at the end of the inserted text");
}

void TestCommandParsing() {
    const patchwork::Command open = patchwork::ParseCommand(":open src/main.cpp");
    const patchwork::Command accept_all = patchwork::ParseCommand(":patch accept-all");
    const patchwork::Command invalid = patchwork::ParseCommand(":patch nope");

    Expect(open.type == patchwork::CommandType::Open, "open command should parse");
    Expect(open.argument == "src/main.cpp", "open command should preserve the path");
    Expect(accept_all.type == patchwork::CommandType::PatchAcceptAll, "accept-all should parse");
    Expect(invalid.type == patchwork::CommandType::Invalid, "invalid command should be rejected");
}

void TestDiffParsingAndPatchApply() {
    patchwork::Buffer buffer = patchwork::LoadFileBuffer("sample.cpp");
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {\n    int totalBefore;\n    return totalBefore;\n}", false);

    const std::string diff_text =
        "--- a/sample.cpp\n"
        "+++ b/sample.cpp\n"
        "@@ -1,4 +1,4 @@\n"
        " int main() {\n"
        "-    int totalBefore;\n"
        "+    int totalBefore = 0;\n"
        "     return totalBefore;\n"
        " }\n";

    const patchwork::PatchSet patch = patchwork::ParseUnifiedDiff(diff_text);
    Expect(patch.valid(), "valid diff should parse");
    Expect(patch.hunks.size() == 1, "single hunk expected");
    Expect(patchwork::PatchTargetsBuffer(patch, buffer), "patch should target the current buffer");

    patchwork::PatchSession session = patchwork::CreatePatchSession(patch, buffer);
    const patchwork::PatchApplyResult result = patchwork::AcceptCurrentHunk(buffer, session);
    Expect(result.success, "current hunk should apply");
    Expect(buffer.text().find("int totalBefore = 0;") != std::string::npos,
           "accepted hunk should update the file buffer");
}

void TestDiffExtractionWithProse() {
    const std::string raw =
        "Here is the minimal fix.\n\n"
        "--- a/sample.cpp\n"
        "+++ b/sample.cpp\n"
        "@@ -1,2 +1,2 @@\n"
        "-int totalBefore;\n"
        "+int totalBefore = 0;\n"
        "\n"
        "This initializes the variable.\n";

    const std::string extracted = patchwork::ExtractDiffText(raw);
    Expect(extracted.find("This initializes the variable.") == std::string::npos,
           "diff extraction should stop before trailing prose");
    const patchwork::PatchSet patch = patchwork::ParseUnifiedDiff(extracted);
    Expect(patch.valid(), "extracted diff should remain parseable");
}

void TestBuildRunner() {
    const patchwork::BuildResult result = patchwork::RunBuildCommand("sh -c 'echo boom; exit 7'");
    Expect(result.ran, "build command should run");
    Expect(result.exit_code == 7, "build command should preserve exit code");
    Expect(result.output.find("boom") != std::string::npos, "build output should be captured");
}

bool HasSpan(const std::vector<patchwork::SyntaxSpan>& spans,
             size_t start,
             size_t end,
             patchwork::SyntaxTokenKind kind) {
    for (const patchwork::SyntaxSpan& span : spans) {
        if (span.start == start && span.end == end && span.kind == kind) {
            return true;
        }
    }
    return false;
}

void TestLanguageDetection() {
    patchwork::Buffer cpp_buffer;
    cpp_buffer.setPath("sample.cpp");
    Expect(cpp_buffer.languageId() == patchwork::LanguageId::Cpp, "cpp files should detect as C++");
    Expect(cpp_buffer.guessLanguage() == "C++", "cpp files should show the C++ label");

    patchwork::Buffer rust_buffer;
    rust_buffer.setPath("sample.rs");
    Expect(rust_buffer.languageId() == patchwork::LanguageId::Rust, "rust files should detect as Rust");
    Expect(rust_buffer.guessLanguage() == "Rust", "rust files should show the Rust label");

    patchwork::Buffer python_buffer;
    python_buffer.setPath("sample.py");
    Expect(python_buffer.languageId() == patchwork::LanguageId::Python, "python files should detect as Python");
    Expect(python_buffer.guessLanguage() == "Python", "python files should show the Python label");

    patchwork::Buffer javascript_buffer;
    javascript_buffer.setPath("sample.js");
    Expect(javascript_buffer.languageId() == patchwork::LanguageId::JavaScript,
           "javascript files should detect as JavaScript");
    Expect(javascript_buffer.guessLanguage() == "JavaScript",
           "javascript files should show the JavaScript label");

    patchwork::Buffer typescript_buffer;
    typescript_buffer.setPath("sample.ts");
    Expect(typescript_buffer.languageId() == patchwork::LanguageId::TypeScript,
           "typescript files should detect as TypeScript");
    Expect(typescript_buffer.guessLanguage() == "TypeScript",
           "typescript files should show the TypeScript label");

    patchwork::Buffer java_buffer;
    java_buffer.setPath("sample.java");
    Expect(java_buffer.languageId() == patchwork::LanguageId::Java, "java files should detect as Java");
    Expect(java_buffer.guessLanguage() == "Java", "java files should show the Java label");

    patchwork::Buffer go_buffer;
    go_buffer.setPath("sample.go");
    Expect(go_buffer.languageId() == patchwork::LanguageId::Go, "go files should detect as Go");
    Expect(go_buffer.guessLanguage() == "Go", "go files should show the Go label");

    patchwork::Buffer markdown_buffer;
    markdown_buffer.setPath("notes.md");
    Expect(markdown_buffer.languageId() == patchwork::LanguageId::Markdown,
           "markdown files should detect as Markdown");
    Expect(markdown_buffer.guessLanguage() == "Markdown",
           "markdown files should show the Markdown label");

    patchwork::Buffer header_buffer;
    header_buffer.setPath("sample.h");
    Expect(header_buffer.languageId() == patchwork::LanguageId::CHeader,
           "header files should keep the shared C/C++ language id");
    Expect(header_buffer.guessLanguage() == "C/C++", "header files should keep the C/C++ status label");

    patchwork::Buffer text_buffer;
    text_buffer.setPath("notes.custom");
    Expect(text_buffer.languageId() == patchwork::LanguageId::PlainText,
           "unknown extensions should fall back to plain text");
}

void TestCppHighlighterSpans() {
    patchwork::CppHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine("#include <iostream> // stream support", {}, &spans);
    Expect(state.value == 0, "single-line comments should not carry state");
    Expect(HasSpan(spans, 0, 8, patchwork::SyntaxTokenKind::Preprocessor),
           "include directive should be tokenized as preprocessor");
    Expect(HasSpan(spans, 9, 19, patchwork::SyntaxTokenKind::IncludePath),
           "include target should be tokenized separately");
    Expect(HasSpan(spans, 20, 37, patchwork::SyntaxTokenKind::Comment),
           "trailing line comment should be tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("/* block comment", {}, &spans);
    Expect(state.value != 0, "unterminated block comments should carry state to the next line");
    Expect(HasSpan(spans, 0, 16, patchwork::SyntaxTokenKind::Comment),
           "block comment start should be tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("continues here */", state, &spans);
    Expect(state.value == 0, "closed block comments should clear the carried state");
    Expect(HasSpan(spans, 0, 17, patchwork::SyntaxTokenKind::Comment),
           "continued block comments should stay tokenized as comment");

    spans.clear();
    state = highlighter.HighlightLine("#define MAX_COUNT 0x2A", {}, &spans);
    Expect(HasSpan(spans, 0, 7, patchwork::SyntaxTokenKind::Preprocessor),
           "preprocessor directives should stay tokenized as preprocessor");
    Expect(HasSpan(spans, 8, 17, patchwork::SyntaxTokenKind::Macro),
           "macro names should be tokenized separately");
    Expect(HasSpan(spans, 18, 22, patchwork::SyntaxTokenKind::Number),
           "numeric literals should be tokenized");

    spans.clear();
    state = highlighter.HighlightLine("constexpr auto value = ComputeValue(0x2A, \"hi\", 'x', true);", {}, &spans);
    Expect(HasSpan(spans, 0, 9, patchwork::SyntaxTokenKind::Keyword),
           "constexpr should be tokenized as a keyword");
    Expect(HasSpan(spans, 10, 14, patchwork::SyntaxTokenKind::Type),
           "auto should be tokenized as a type keyword");
    Expect(HasSpan(spans, 23, 35, patchwork::SyntaxTokenKind::Function),
           "function identifiers should be tokenized when followed by a call");
    Expect(HasSpan(spans, 36, 40, patchwork::SyntaxTokenKind::Number),
           "hex numeric literals should be tokenized");
    Expect(HasSpan(spans, 42, 46, patchwork::SyntaxTokenKind::String),
           "string literals should be tokenized");
    Expect(HasSpan(spans, 48, 51, patchwork::SyntaxTokenKind::String),
           "character literals should be tokenized");
    Expect(HasSpan(spans, 53, 57, patchwork::SyntaxTokenKind::Keyword),
           "boolean literals should be tokenized as keywords");

    spans.clear();
    state = highlighter.HighlightLine("class Widget final : public Base {", {}, &spans);
    Expect(HasSpan(spans, 0, 5, patchwork::SyntaxTokenKind::Keyword),
           "class should be tokenized as a keyword");
    Expect(HasSpan(spans, 6, 12, patchwork::SyntaxTokenKind::Type),
           "declared type names should be tokenized as types");
    Expect(HasSpan(spans, 13, 18, patchwork::SyntaxTokenKind::Keyword),
           "final should be tokenized as a keyword");
    Expect(HasSpan(spans, 21, 27, patchwork::SyntaxTokenKind::Keyword),
           "access specifiers should be tokenized as keywords");
}

void TestRustHighlighterSpans() {
    patchwork::RustHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state =
        highlighter.HighlightLine("pub fn render_value(input: i32) -> String { println!(\"{}\", 0x2A); } // comment",
                                  {},
                                  &spans);
    Expect(state.value == 0, "single-line rust constructs should not carry state");
    Expect(HasSpan(spans, 0, 3, patchwork::SyntaxTokenKind::Keyword),
           "pub should be tokenized as a keyword");
    Expect(HasSpan(spans, 4, 6, patchwork::SyntaxTokenKind::Keyword),
           "fn should be tokenized as a keyword");
    Expect(HasSpan(spans, 7, 19, patchwork::SyntaxTokenKind::Function),
           "declared rust function names should be tokenized as functions");
    Expect(HasSpan(spans, 27, 30, patchwork::SyntaxTokenKind::Type),
           "primitive rust types should be tokenized as types");
    Expect(HasSpan(spans, 35, 41, patchwork::SyntaxTokenKind::Type),
           "Rust type names should be tokenized as types");
    Expect(HasSpan(spans, 44, 51, patchwork::SyntaxTokenKind::Macro),
           "macro invocations should be tokenized as macros");
    Expect(HasSpan(spans, 53, 57, patchwork::SyntaxTokenKind::String),
           "string literals should be tokenized");
    Expect(HasSpan(spans, 59, 63, patchwork::SyntaxTokenKind::Number),
           "numeric literals should be tokenized");
    Expect(HasSpan(spans, 68, 78, patchwork::SyntaxTokenKind::Comment),
           "line comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("/* outer /* inner */ still", {}, &spans);
    Expect(state.value != 0, "nested rust block comments should carry state to the next line");
    Expect(HasSpan(spans, 0, 26, patchwork::SyntaxTokenKind::Comment),
           "block comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("comment */ let value = r#\"hi", state, &spans);
    Expect(state.value != 0, "unterminated raw rust strings should carry state");
    Expect(HasSpan(spans, 0, 10, patchwork::SyntaxTokenKind::Comment),
           "continued nested block comments should stay comments");
    Expect(HasSpan(spans, 23, 28, patchwork::SyntaxTokenKind::String),
           "raw rust strings should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("there\"#; let answer = 42usize;", state, &spans);
    Expect(state.value == 0, "closed raw rust strings should clear carried state");
    Expect(HasSpan(spans, 0, 7, patchwork::SyntaxTokenKind::String),
           "continued raw rust strings should stay tokenized as strings");
    Expect(HasSpan(spans, 22, 29, patchwork::SyntaxTokenKind::Number),
           "rust numeric suffixes should remain part of the number token");
}

void TestPythonHighlighterSpans() {
    patchwork::PythonHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state =
        highlighter.HighlightLine("@decorator\n"
                                  "async def render_value(value: int) -> str:\n",
                                  {},
                                  nullptr);
    Expect(state.value == 0, "python lexer should not carry state across complete lines");

    spans.clear();
    state = highlighter.HighlightLine("@decorator", {}, &spans);
    Expect(HasSpan(spans, 0, 10, patchwork::SyntaxTokenKind::Macro),
           "python decorators should be tokenized as macros");

    spans.clear();
    state = highlighter.HighlightLine(
        "async def render_value(value: int) -> str: return format_value(0x2A, \"hi\") # note", {}, &spans);
    Expect(HasSpan(spans, 0, 5, patchwork::SyntaxTokenKind::Keyword),
           "async should be tokenized as a keyword");
    Expect(HasSpan(spans, 6, 9, patchwork::SyntaxTokenKind::Keyword),
           "def should be tokenized as a keyword");
    Expect(HasSpan(spans, 10, 22, patchwork::SyntaxTokenKind::Function),
           "declared python function names should be tokenized as functions");
    Expect(HasSpan(spans, 30, 33, patchwork::SyntaxTokenKind::Type),
           "python builtin parameter types should be tokenized as types");
    Expect(HasSpan(spans, 38, 41, patchwork::SyntaxTokenKind::Type),
           "python return types should be tokenized as types");
    Expect(HasSpan(spans, 43, 49, patchwork::SyntaxTokenKind::Keyword),
           "return should be tokenized as a keyword");
    Expect(HasSpan(spans, 50, 62, patchwork::SyntaxTokenKind::Function),
           "python function calls should be tokenized as functions");
    Expect(HasSpan(spans, 63, 67, patchwork::SyntaxTokenKind::Number),
           "python numeric literals should be tokenized");
    Expect(HasSpan(spans, 69, 73, patchwork::SyntaxTokenKind::String),
           "python string literals should be tokenized");
    Expect(HasSpan(spans, 75, 81, patchwork::SyntaxTokenKind::Comment),
           "python comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("text = \"\"\"hello", {}, &spans);
    Expect(state.value != 0, "unterminated triple-quoted python strings should carry state");
    Expect(HasSpan(spans, 7, 15, patchwork::SyntaxTokenKind::String),
           "python triple-quoted string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world\"\"\"", state, &spans);
    Expect(state.value == 0, "closed triple-quoted python strings should clear carried state");
    Expect(HasSpan(spans, 0, 8, patchwork::SyntaxTokenKind::String),
           "continued triple-quoted python strings should remain tokenized as strings");
}

void TestJavaScriptHighlighterSpans() {
    patchwork::JavaScriptHighlighter highlighter(patchwork::LanguageId::JavaScript);
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine(
        "export async function renderValue(input) { return formatValue(0x2A, \"hi\"); } // note", {}, &spans);
    Expect(state.value == 0, "single-line javascript constructs should not carry state");
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::Keyword),
           "export should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 7, 12, patchwork::SyntaxTokenKind::Keyword),
           "async should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 13, 21, patchwork::SyntaxTokenKind::Keyword),
           "function should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 22, 33, patchwork::SyntaxTokenKind::Function),
           "declared javascript function names should be tokenized as functions");
    Expect(HasSpan(spans, 43, 49, patchwork::SyntaxTokenKind::Keyword),
           "return should be tokenized as a javascript keyword");
    Expect(HasSpan(spans, 50, 61, patchwork::SyntaxTokenKind::Function),
           "javascript function calls should be tokenized as functions");
    Expect(HasSpan(spans, 62, 66, patchwork::SyntaxTokenKind::Number),
           "javascript numeric literals should be tokenized");
    Expect(HasSpan(spans, 68, 72, patchwork::SyntaxTokenKind::String),
           "javascript string literals should be tokenized");
    Expect(HasSpan(spans, 77, 84, patchwork::SyntaxTokenKind::Comment),
           "javascript comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("const message = `hello", {}, &spans);
    Expect(state.value != 0, "unterminated javascript template strings should carry state");
    Expect(HasSpan(spans, 16, 22, patchwork::SyntaxTokenKind::String),
           "javascript template string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world`;", state, &spans);
    Expect(state.value == 0, "closed javascript template strings should clear carried state");
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::String),
           "continued javascript template strings should remain tokenized as strings");
}

void TestTypeScriptHighlighterSpans() {
    patchwork::JavaScriptHighlighter highlighter(patchwork::LanguageId::TypeScript);
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine("interface WidgetProps { title: string }", {}, &spans);
    Expect(state.value == 0, "single-line typescript constructs should not carry state");
    Expect(HasSpan(spans, 0, 9, patchwork::SyntaxTokenKind::Keyword),
           "interface should be tokenized as a typescript keyword");
    Expect(HasSpan(spans, 10, 21, patchwork::SyntaxTokenKind::Type),
           "declared typescript interface names should be tokenized as types");
    Expect(HasSpan(spans, 31, 37, patchwork::SyntaxTokenKind::Type),
           "typescript annotation types should be tokenized as types");

    spans.clear();
    state = highlighter.HighlightLine(
        "function renderValue(value: number): Promise<string> { return formatValue(value as number); }",
        {},
        &spans);
    Expect(HasSpan(spans, 0, 8, patchwork::SyntaxTokenKind::Keyword),
           "function should stay tokenized as a typescript keyword");
    Expect(HasSpan(spans, 9, 20, patchwork::SyntaxTokenKind::Function),
           "declared typescript function names should be tokenized as functions");
    Expect(HasSpan(spans, 28, 34, patchwork::SyntaxTokenKind::Type),
           "typescript parameter annotation types should be tokenized");
    Expect(HasSpan(spans, 37, 44, patchwork::SyntaxTokenKind::Type),
           "typescript generic container types should be tokenized");
    Expect(HasSpan(spans, 45, 51, patchwork::SyntaxTokenKind::Type),
           "typescript generic parameter types should be tokenized");
    Expect(HasSpan(spans, 55, 61, patchwork::SyntaxTokenKind::Keyword),
           "return should be tokenized as a typescript keyword");
    Expect(HasSpan(spans, 62, 73, patchwork::SyntaxTokenKind::Function),
           "typescript function calls should be tokenized as functions");
    Expect(HasSpan(spans, 80, 82, patchwork::SyntaxTokenKind::Keyword),
           "typescript as-casts should keep the as keyword highlighted");
    Expect(HasSpan(spans, 83, 89, patchwork::SyntaxTokenKind::Type),
           "typescript cast target types should be tokenized");
}

void TestJavaHighlighterSpans() {
    patchwork::JavaHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine("@Override", {}, &spans);
    Expect(state.value == 0, "single-line java annotations should not carry state");
    Expect(HasSpan(spans, 0, 9, patchwork::SyntaxTokenKind::Macro),
           "java annotations should be tokenized as macros");

    spans.clear();
    state = highlighter.HighlightLine(
        "public String renderValue(int count) { return formatValue(0x2A, \"hi\"); } // note", {}, &spans);
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::Keyword),
           "public should be tokenized as a java keyword");
    Expect(HasSpan(spans, 7, 13, patchwork::SyntaxTokenKind::Type),
           "java return types should be tokenized as types");
    Expect(HasSpan(spans, 14, 25, patchwork::SyntaxTokenKind::Function),
           "declared java method names should be tokenized as functions");
    Expect(HasSpan(spans, 26, 29, patchwork::SyntaxTokenKind::Type),
           "java primitive parameter types should be tokenized");
    Expect(HasSpan(spans, 39, 45, patchwork::SyntaxTokenKind::Keyword),
           "return should be tokenized as a java keyword");
    Expect(HasSpan(spans, 46, 57, patchwork::SyntaxTokenKind::Function),
           "java method calls should be tokenized as functions");
    Expect(HasSpan(spans, 58, 62, patchwork::SyntaxTokenKind::Number),
           "java numeric literals should be tokenized");
    Expect(HasSpan(spans, 64, 68, patchwork::SyntaxTokenKind::String),
           "java string literals should be tokenized");
    Expect(HasSpan(spans, 73, 80, patchwork::SyntaxTokenKind::Comment),
           "java comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("public class Widget extends Base {", {}, &spans);
    Expect(HasSpan(spans, 7, 12, patchwork::SyntaxTokenKind::Keyword),
           "class should be tokenized as a java keyword");
    Expect(HasSpan(spans, 13, 19, patchwork::SyntaxTokenKind::Type),
           "declared java class names should be tokenized as types");
    Expect(HasSpan(spans, 20, 27, patchwork::SyntaxTokenKind::Keyword),
           "extends should be tokenized as a java keyword");
    Expect(HasSpan(spans, 28, 32, patchwork::SyntaxTokenKind::Type),
           "extended java types should be tokenized as types");

    spans.clear();
    state = highlighter.HighlightLine("String text = \"\"\"hello", {}, &spans);
    Expect(state.value != 0, "unterminated java text blocks should carry state");
    Expect(HasSpan(spans, 14, 22, patchwork::SyntaxTokenKind::String),
           "java text block starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world\"\"\";", state, &spans);
    Expect(state.value == 0, "closed java text blocks should clear carried state");
    Expect(HasSpan(spans, 0, 8, patchwork::SyntaxTokenKind::String),
           "continued java text blocks should remain tokenized as strings");
}

void TestGoHighlighterSpans() {
    patchwork::GoHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine(
        "func renderValue(count int) string { return formatValue(0x2A, \"hi\") } // note", {}, &spans);
    Expect(state.value == 0, "single-line go constructs should not carry state");
    Expect(HasSpan(spans, 0, 4, patchwork::SyntaxTokenKind::Keyword),
           "func should be tokenized as a go keyword");
    Expect(HasSpan(spans, 5, 16, patchwork::SyntaxTokenKind::Function),
           "declared go function names should be tokenized as functions");
    Expect(HasSpan(spans, 23, 26, patchwork::SyntaxTokenKind::Type),
           "go parameter types should be tokenized as types");
    Expect(HasSpan(spans, 28, 34, patchwork::SyntaxTokenKind::Type),
           "go return types should be tokenized as types");
    Expect(HasSpan(spans, 37, 43, patchwork::SyntaxTokenKind::Keyword),
           "return should be tokenized as a go keyword");
    Expect(HasSpan(spans, 44, 55, patchwork::SyntaxTokenKind::Function),
           "go function calls should be tokenized as functions");
    Expect(HasSpan(spans, 56, 60, patchwork::SyntaxTokenKind::Number),
           "go numeric literals should be tokenized");
    Expect(HasSpan(spans, 62, 66, patchwork::SyntaxTokenKind::String),
           "go string literals should be tokenized");
    Expect(HasSpan(spans, 70, 77, patchwork::SyntaxTokenKind::Comment),
           "go comments should be tokenized as comments");

    spans.clear();
    state = highlighter.HighlightLine("type Widget struct {}", {}, &spans);
    Expect(HasSpan(spans, 0, 4, patchwork::SyntaxTokenKind::Keyword),
           "type should be tokenized as a go keyword");
    Expect(HasSpan(spans, 5, 11, patchwork::SyntaxTokenKind::Type),
           "declared go type names should be tokenized as types");
    Expect(HasSpan(spans, 12, 18, patchwork::SyntaxTokenKind::Keyword),
           "struct should be tokenized as a go keyword");

    spans.clear();
    state = highlighter.HighlightLine("message := `hello", {}, &spans);
    Expect(state.value != 0, "unterminated go raw strings should carry state");
    Expect(HasSpan(spans, 11, 17, patchwork::SyntaxTokenKind::String),
           "go raw string starts should be tokenized as strings");

    spans.clear();
    state = highlighter.HighlightLine("world`", state, &spans);
    Expect(state.value == 0, "closed go raw strings should clear carried state");
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::String),
           "continued go raw strings should remain tokenized as strings");
}

void TestMarkdownHighlighterSpans() {
    patchwork::MarkdownHighlighter highlighter;
    std::vector<patchwork::SyntaxSpan> spans;

    patchwork::SyntaxLineState state = highlighter.HighlightLine("# Heading", {}, &spans);
    Expect(state.value == 0, "single-line markdown headings should not carry state");
    Expect(HasSpan(spans, 0, 9, patchwork::SyntaxTokenKind::Heading),
           "markdown headings should be tokenized as headings");

    spans.clear();
    state = highlighter.HighlightLine("> quoted", {}, &spans);
    Expect(HasSpan(spans, 0, 8, patchwork::SyntaxTokenKind::Quote),
           "markdown blockquotes should be tokenized as quotes");

    spans.clear();
    state = highlighter.HighlightLine("- [x] item", {}, &spans);
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::ListMarker),
           "markdown task list prefixes should be tokenized as list markers");

    spans.clear();
    state = highlighter.HighlightLine("Use `code` [docs](url) *style*", {}, &spans);
    Expect(HasSpan(spans, 4, 10, patchwork::SyntaxTokenKind::InlineCode),
           "markdown inline code should be tokenized");
    Expect(HasSpan(spans, 11, 17, patchwork::SyntaxTokenKind::LinkText),
           "markdown link text should be tokenized");
    Expect(HasSpan(spans, 17, 22, patchwork::SyntaxTokenKind::LinkUrl),
           "markdown link urls should be tokenized");
    Expect(HasSpan(spans, 23, 30, patchwork::SyntaxTokenKind::Emphasis),
           "markdown emphasis should be tokenized");

    spans.clear();
    state = highlighter.HighlightLine("```cpp", {}, &spans);
    Expect(state.value != 0, "opening markdown code fences should carry state");
    Expect(HasSpan(spans, 0, 6, patchwork::SyntaxTokenKind::CodeFence),
           "markdown code fence lines should be tokenized as code fences");

    spans.clear();
    state = highlighter.HighlightLine("int main() {", state, &spans);
    Expect(HasSpan(spans, 0, 3, patchwork::SyntaxTokenKind::Type),
           "fenced markdown code should delegate to the nested language highlighter for types");
    Expect(HasSpan(spans, 4, 8, patchwork::SyntaxTokenKind::Function),
           "fenced markdown code should delegate to the nested language highlighter for functions");

    spans.clear();
    state = highlighter.HighlightLine("```", state, &spans);
    Expect(state.value == 0, "closing markdown code fences should clear carried state");
    Expect(HasSpan(spans, 0, 3, patchwork::SyntaxTokenKind::CodeFence),
           "closing markdown code fences should stay tokenized as code fences");
}

void TestIncludeHighlightRendering() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("#include <iostream> // stream support\n"
                   "/* block comment\n"
                   "continues here */\n"
                   "int main() {}",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 80);

    Expect(rendered.find("1\xE2\x94\x82 ") != std::string::npos, "file view should render line numbers");
    Expect(rendered.find("2\xE2\x94\x82 ") != std::string::npos,
           "multi-line buffers should render later line numbers");
    Expect(rendered.find("\x1b[38;5;141m#include\x1b[39m") != std::string::npos,
           "include directive should be purple");
    Expect(rendered.find("\x1b[38;5;214m<iostream>\x1b[39m") != std::string::npos,
           "include target should be orange");
    Expect(rendered.find("\x1b[38;5;30m// stream support\x1b[39m") != std::string::npos,
           "line comments should be dark teal");
    Expect(rendered.find("\x1b[38;5;30m/* block comment\x1b[39m") != std::string::npos,
           "block comment start should be dark teal");
    Expect(rendered.find("\x1b[38;5;30mcontinues here */\x1b[39m") != std::string::npos,
           "continued block comments should stay dark teal");
}

void TestCppRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("#define MAX_COUNT 0x2A\n"
                   "constexpr auto value = ComputeValue(0x2A, \"hi\", 'x', true);\n"
                   "class Widget final : public Base {}",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220mMAX_COUNT\x1b[39m") != std::string::npos,
           "macro names should render with the macro color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "numeric literals should render with the number color");
    Expect(rendered.find("\x1b[38;5;75mconstexpr\x1b[39m") != std::string::npos,
           "keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;81mauto\x1b[39m") != std::string::npos,
           "type keywords should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mComputeValue\x1b[39m") != std::string::npos,
           "function identifiers should render with the function color");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "string literals should render with the string color");
    Expect(rendered.find("\x1b[38;5;221m'x'\x1b[39m") != std::string::npos,
           "character literals should render with the string color");
    Expect(rendered.find("\x1b[38;5;81mWidget\x1b[39m") != std::string::npos,
           "declared type names should render with the type color");
}

void TestRustRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.rs");
    buffer.setText("pub fn render_value(input: i32) -> String {\n"
                   "    println!(\"{}\", 0x2A);\n"
                   "}\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mpub\x1b[39m") != std::string::npos,
           "rust keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;117mrender_value\x1b[39m") != std::string::npos,
           "rust function declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;81mi32\x1b[39m") != std::string::npos,
           "rust primitive types should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mString\x1b[39m") != std::string::npos,
           "rust type names should render with the type color");
    Expect(rendered.find("\x1b[38;5;220mprintln\x1b[39m") != std::string::npos,
           "rust macros should render with the macro color");
    Expect(rendered.find("\x1b[38;5;221m\"{}\"\x1b[39m") != std::string::npos,
           "rust strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "rust numbers should render with the number color");
}

void TestPythonRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.py");
    buffer.setText("@decorator\n"
                   "async def render_value(value: int) -> str:\n"
                   "    return format_value(0x2A, \"hi\")  # note\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220m@decorator\x1b[39m") != std::string::npos,
           "python decorators should render with the macro color");
    Expect(rendered.find("\x1b[38;5;75masync\x1b[39m") != std::string::npos,
           "python keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;117mrender_value\x1b[39m") != std::string::npos,
           "python function declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;81mint\x1b[39m") != std::string::npos,
           "python builtin types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mformat_value\x1b[39m") != std::string::npos,
           "python function calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "python numeric literals should render with the number color");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "python strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m# note\x1b[39m") != std::string::npos,
           "python comments should render with the comment color");
}

void TestJavaScriptRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.js");
    buffer.setText("export async function renderValue(input) {\n"
                   "    return formatValue(0x2A, \"hi\"); // note\n"
                   "}\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mexport\x1b[39m") != std::string::npos,
           "javascript keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;75masync\x1b[39m") != std::string::npos,
           "javascript async should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;117mrenderValue\x1b[39m") != std::string::npos,
           "javascript function declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;117mformatValue\x1b[39m") != std::string::npos,
           "javascript function calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "javascript numbers should render with the number color");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "javascript strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "javascript comments should render with the comment color");
}

void TestTypeScriptRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.ts");
    buffer.setText("interface WidgetProps { title: string }\n"
                   "function renderValue(value: number): Promise<string> {\n"
                   "    return formatValue(value as number);\n"
                   "}\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75minterface\x1b[39m") != std::string::npos,
           "typescript keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;81mWidgetProps\x1b[39m") != std::string::npos,
           "typescript declared interface names should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mstring\x1b[39m") != std::string::npos,
           "typescript annotation types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mrenderValue\x1b[39m") != std::string::npos,
           "typescript function declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;81mnumber\x1b[39m") != std::string::npos,
           "typescript numeric annotation types should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mPromise\x1b[39m") != std::string::npos,
           "typescript generic container types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mformatValue\x1b[39m") != std::string::npos,
           "typescript function calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;75mas\x1b[39m") != std::string::npos,
           "typescript casts should keep the as keyword highlighted");
}

void TestJavaRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.java");
    buffer.setText("@Override\n"
                   "public class Widget extends Base {\n"
                   "    public String renderValue(int count) {\n"
                   "        return formatValue(0x2A, \"hi\"); // note\n"
                   "    }\n"
                   "}\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;220m@Override\x1b[39m") != std::string::npos,
           "java annotations should render with the macro color");
    Expect(rendered.find("\x1b[38;5;75mpublic\x1b[39m") != std::string::npos,
           "java keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;81mWidget\x1b[39m") != std::string::npos,
           "java declared class names should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mBase\x1b[39m") != std::string::npos,
           "java extended type names should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mString\x1b[39m") != std::string::npos,
           "java builtin reference types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mrenderValue\x1b[39m") != std::string::npos,
           "java method declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;81mint\x1b[39m") != std::string::npos,
           "java primitive types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mformatValue\x1b[39m") != std::string::npos,
           "java method calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "java numbers should render with the number color");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "java strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "java comments should render with the comment color");
}

void TestGoRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.go");
    buffer.setText("package sample\n"
                   "type Widget struct {}\n"
                   "func renderValue(count int) string {\n"
                   "    return formatValue(0x2A, \"hi\") // note\n"
                   "}\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[38;5;75mpackage\x1b[39m") != std::string::npos,
           "go keywords should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;81mWidget\x1b[39m") != std::string::npos,
           "go declared type names should render with the type color");
    Expect(rendered.find("\x1b[38;5;75mfunc\x1b[39m") != std::string::npos,
           "go func should render with the keyword color");
    Expect(rendered.find("\x1b[38;5;117mrenderValue\x1b[39m") != std::string::npos,
           "go function declarations should render with the function color");
    Expect(rendered.find("\x1b[38;5;81mint\x1b[39m") != std::string::npos,
           "go primitive types should render with the type color");
    Expect(rendered.find("\x1b[38;5;81mstring\x1b[39m") != std::string::npos,
           "go return types should render with the type color");
    Expect(rendered.find("\x1b[38;5;117mformatValue\x1b[39m") != std::string::npos,
           "go function calls should render with the function color");
    Expect(rendered.find("\x1b[38;5;179m0x2A\x1b[39m") != std::string::npos,
           "go numbers should render with the number color");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "go strings should render with the string color");
    Expect(rendered.find("\x1b[38;5;30m// note\x1b[39m") != std::string::npos,
           "go comments should render with the comment color");
}

void TestMarkdownRenderHighlightsExpandedTokenSet() {
    patchwork::Buffer buffer;
    buffer.setPath("notes.md");
    buffer.setText("# Heading\n"
                   "> quote\n"
                   "- [x] item\n"
                   "Use `code` and [docs](https://example.com) with *style*\n"
                   "```python\n"
                   "def render_value():\n"
                   "    return \"hi\"\n"
                   "```\n",
                   false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 12, 120);

    Expect(rendered.find("\x1b[38;5;111m# Heading\x1b[39m") != std::string::npos,
           "markdown headings should render with the heading color");
    Expect(rendered.find("\x1b[38;5;66m> quote\x1b[39m") != std::string::npos,
           "markdown blockquotes should render with the quote color");
    Expect(rendered.find("\x1b[38;5;215m- [x] \x1b[39m") != std::string::npos,
           "markdown list markers should render with the list marker color");
    Expect(rendered.find("\x1b[38;5;220m`code`\x1b[39m") != std::string::npos,
           "markdown inline code should render with the inline code color");
    Expect(rendered.find("\x1b[38;5;117m[docs]\x1b[38;5;214m(") != std::string::npos,
           "markdown link text should render with the link text color");
    Expect(rendered.find("\x1b[38;5;214m(https://example.com)\x1b[39m") != std::string::npos,
           "markdown link urls should render with the link url color");
    Expect(rendered.find("\x1b[38;5;211m*style*\x1b[39m") != std::string::npos,
           "markdown emphasis should render with the emphasis color");
    Expect(rendered.find("\x1b[38;5;141m```python\x1b[39m") != std::string::npos,
           "markdown code fences should render with the code fence color");
    Expect(rendered.find("\x1b[38;5;75mdef\x1b[39m") != std::string::npos,
           "markdown fenced code should render nested language keywords");
    Expect(rendered.find("\x1b[38;5;117mrender_value\x1b[39m") != std::string::npos,
           "markdown fenced code should render nested language functions");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "markdown fenced code should render nested language strings");
}

void TestLineNumberGutterAffectsVisibleWidth() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("abcdefghijklmnopqrstuvwxyz", false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 10);

    Expect(rendered.find("1\xE2\x94\x82 abcdefg") != std::string::npos,
           "line number gutter should reduce visible content width");
    Expect(screen.ContentColumns(state, 10) == 7, "content width should subtract the line number gutter");
}

void TestAiScratchDoesNotRenderLineNumbers() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {}", false);

    patchwork::EditorState state(std::move(buffer));
    state.setAiText("Explaining selection via codex");
    state.setActiveView(patchwork::ViewKind::AiScratch);

    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 20);

    Expect(rendered.find("1\xE2\x94\x82 ") == std::string::npos,
           "AI scratch should not render file line numbers");
    Expect(rendered.find("Explaining selection") != std::string::npos,
           "AI scratch should use the full line width for response text");
    Expect(screen.ContentColumns(state, 20) == 20, "AI scratch width should not reserve gutter space");
}

void TestAiScratchDiffHunksUseFileSyntaxHighlighting() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int main() {}", false);

    patchwork::EditorState state(std::move(buffer));
    state.setAiText("Here is the patch:\n\n"
                    "@@ -1,1 +1,1 @@\n"
                    "-int total = 0;\n"
                    "+int total = ComputeValue(0x2A, \"hi\");\n");
    state.setActiveView(patchwork::ViewKind::AiScratch);

    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[36m@@ -1,1 +1,1 @@\x1b[39m") != std::string::npos,
           "AI diff hunk headers should still be decorated");
    Expect(rendered.find("\x1b[38;5;81mint\x1b[39m") != std::string::npos,
           "AI diff code should use the file syntax policy for types");
    Expect(rendered.find("\x1b[38;5;117mComputeValue\x1b[39m") != std::string::npos,
           "AI diff code should highlight function identifiers");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "AI diff code should highlight string literals");
}

void TestPatchPreviewAddedLinesUseFileSyntaxHighlighting() {
    patchwork::Buffer buffer;
    buffer.setPath("sample.cpp");
    buffer.setText("int total = 0;\n", false);

    const std::string diff_text =
        "@@ -1,1 +1,1 @@\n"
        "-int total = 0;\n"
        "+int total = ComputeValue(0x2A, \"hi\");\n";

    const patchwork::PatchSet patch = patchwork::ParseUnifiedDiff(diff_text);
    patchwork::EditorState state(std::move(buffer));
    state.setPatchSession(patchwork::CreatePatchSession(patch, state.fileBuffer()));
    state.setActiveView(patchwork::ViewKind::PatchPreview);

    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 8, 120);

    Expect(rendered.find("\x1b[36m@@ -1,1 +1,1 @@ [PENDING]\x1b[39m") != std::string::npos,
           "patch preview hunk headers should stay decorated");
    Expect(rendered.find("\x1b[31m-int total = 0;\x1b[39m") != std::string::npos,
           "removed lines in patch preview may stay plain red");
    Expect(rendered.find("\x1b[32m+\x1b[39m\x1b[38;5;81mint\x1b[39m") != std::string::npos,
           "added lines in patch preview should use file syntax highlighting");
    Expect(rendered.find("\x1b[38;5;117mComputeValue\x1b[39m") != std::string::npos,
           "patch preview should highlight functions in added lines");
    Expect(rendered.find("\x1b[38;5;221m\"hi\"\x1b[39m") != std::string::npos,
           "patch preview should highlight strings in added lines");
}

void TestPlainTextFallbackAvoidsCppMiscoloring() {
    patchwork::Buffer buffer;
    buffer.setPath("notes.custom");
    buffer.setText("#include <iostream>\n", false);

    patchwork::EditorState state(std::move(buffer));
    patchwork::Screen screen;
    const std::string rendered = screen.Render(state, {}, 4, 80);

    Expect(rendered.find("\x1b[38;5;141m#include\x1b[39m") == std::string::npos,
           "unknown languages should not inherit C++ include highlighting");
    Expect(rendered.find("\x1b[38;5;214m<iostream>\x1b[39m") == std::string::npos,
           "plain-text fallback should avoid include-path miscoloring");
}

void TestMockAiClient() {
    patchwork::MockAiClient client(std::filesystem::path(PATCHWORK_SOURCE_DIR) / "tests" / "fixtures");
    std::string error;
    Expect(client.StartRequest({.kind = patchwork::AiRequestKind::Explain}, &error),
           "mock explain request should start");
    std::vector<patchwork::AiEvent> explain_events = client.PollEvents();
    bool saw_explain_text = false;
    bool saw_explain_complete = false;
    for (const patchwork::AiEvent& event : explain_events) {
        if (event.kind == patchwork::AiEventKind::TextDelta) {
            saw_explain_text = true;
        }
        if (event.kind == patchwork::AiEventKind::Completed) {
            saw_explain_complete = true;
            Expect(event.response.kind == patchwork::AiResponseKind::ExplanationOnly,
                   "explain fixture should be plain text");
        }
    }
    Expect(saw_explain_text, "mock explain should emit text");
    Expect(saw_explain_complete, "mock explain should complete");

    Expect(client.StartRequest({.kind = patchwork::AiRequestKind::Fix}, &error),
           "mock fix request should start");
    std::vector<patchwork::AiEvent> fix_events = client.PollEvents();
    bool saw_fix_complete = false;
    for (const patchwork::AiEvent& event : fix_events) {
        if (event.kind == patchwork::AiEventKind::Completed) {
            saw_fix_complete = true;
            Expect(event.response.diff_text.has_value(), "fix fixture should include a diff");
        }
    }
    Expect(saw_fix_complete, "mock fix should complete");
}

void TestJsonParsing() {
    const std::string payload = R"({"id":7,"method":"item/agentMessage/delta","params":{"delta":"hello\nworld"}})";
    std::string error;
    const std::optional<patchwork::JsonValue> json = patchwork::JsonValue::Parse(payload, &error);
    Expect(json.has_value(), "json payload should parse");
    Expect(json->find("method") != nullptr && json->find("method")->stringValue() == "item/agentMessage/delta",
           "json parser should preserve strings");
    const patchwork::JsonValue* params = json->find("params");
    Expect(params != nullptr && params->find("delta") != nullptr,
           "json parser should expose nested objects");
    Expect(params->find("delta")->stringValue() == "hello\nworld",
           "json parser should unescape newlines");
}

class FakeLocalAgentClient : public patchwork::ILocalAgentClient {
  public:
    bool StartSession(const patchwork::LocalAgentSessionConfig& config,
                      std::string* session_id,
                      std::string* error) override {
        (void)config;
        (void)error;
        current_session_id_ = "session-1";
        if (session_id != nullptr) {
            *session_id = current_session_id_;
        }
        events_.push_back({.kind = patchwork::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Connecting});
        events_.push_back({.kind = patchwork::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Idle});
        return true;
    }

    bool SendMessage(const std::string& session_id,
                     const patchwork::LocalAgentRequest& request,
                     std::string* error) override {
        (void)request;
        (void)error;
        if (session_id != current_session_id_) {
            return false;
        }
        events_.push_back({.kind = patchwork::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Active});
        events_.push_back({.kind = patchwork::LocalAgentEventKind::TextDelta,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Active,
                           .text_delta = "hello"});
        events_.push_back({.kind = patchwork::LocalAgentEventKind::FinalText,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Active,
                           .final_text = "hello"});
        events_.push_back({.kind = patchwork::LocalAgentEventKind::SessionStateChanged,
                           .session_id = current_session_id_,
                           .session_state = patchwork::LocalAgentSessionState::Idle});
        active_ = true;
        return true;
    }

    std::vector<patchwork::LocalAgentEvent> PollEvents() override {
        active_ = false;
        std::vector<patchwork::LocalAgentEvent> result = std::move(events_);
        events_.clear();
        return result;
    }

    bool HasActiveMessage() const override { return active_; }

    void CloseSession(const std::string& session_id) override {
        if (session_id == current_session_id_) {
            current_session_id_.clear();
        }
    }

    void Shutdown() override {
        events_.clear();
        current_session_id_.clear();
        active_ = false;
    }

  private:
    std::string current_session_id_;
    std::vector<patchwork::LocalAgentEvent> events_;
    bool active_ = false;
};

void TestCodexClientIgnoresInitialIdleBeforeFirstTurn() {
    patchwork::CodexClient client(std::make_unique<FakeLocalAgentClient>());
    std::string error;
    Expect(client.StartRequest({.kind = patchwork::AiRequestKind::Explain, .file_path = "sample.cpp"}, &error),
           "codex request should start");

    const std::vector<patchwork::AiEvent> events = client.PollEvents();
    bool saw_completed = false;
    bool saw_error = false;
    bool saw_text_delta = false;
    for (const patchwork::AiEvent& event : events) {
        if (event.kind == patchwork::AiEventKind::TextDelta) {
            saw_text_delta = true;
            Expect(event.text_delta == "hello", "codex adapter should preserve streamed text");
        }
        if (event.kind == patchwork::AiEventKind::Completed) {
            saw_completed = true;
            Expect(event.response.raw_text == "hello", "codex adapter should preserve final text");
        }
        if (event.kind == patchwork::AiEventKind::Error) {
            saw_error = true;
        }
    }

    Expect(saw_text_delta, "codex adapter should emit a text delta");
    Expect(saw_completed, "codex adapter should complete after the first request");
    Expect(!saw_error, "codex adapter should not fail on the initial idle session event");
}

}  // namespace

int main() {
    try {
        TestBufferEditing();
        TestSelectionExtraction();
        TestSelectionRangeHelpers();
        TestBufferRangeEditing();
        TestCommandParsing();
        TestDiffParsingAndPatchApply();
        TestDiffExtractionWithProse();
        TestBuildRunner();
        TestLanguageDetection();
        TestCppHighlighterSpans();
        TestGoHighlighterSpans();
        TestJavaScriptHighlighterSpans();
        TestJavaHighlighterSpans();
        TestMarkdownHighlighterSpans();
        TestPythonHighlighterSpans();
        TestRustHighlighterSpans();
        TestTypeScriptHighlighterSpans();
        TestIncludeHighlightRendering();
        TestCppRenderHighlightsExpandedTokenSet();
        TestGoRenderHighlightsExpandedTokenSet();
        TestJavaScriptRenderHighlightsExpandedTokenSet();
        TestJavaRenderHighlightsExpandedTokenSet();
        TestMarkdownRenderHighlightsExpandedTokenSet();
        TestPythonRenderHighlightsExpandedTokenSet();
        TestRustRenderHighlightsExpandedTokenSet();
        TestTypeScriptRenderHighlightsExpandedTokenSet();
        TestLineNumberGutterAffectsVisibleWidth();
        TestAiScratchDoesNotRenderLineNumbers();
        TestAiScratchDiffHunksUseFileSyntaxHighlighting();
        TestPatchPreviewAddedLinesUseFileSyntaxHighlighting();
        TestPlainTextFallbackAvoidsCppMiscoloring();
        TestMockAiClient();
        TestJsonParsing();
        TestCodexClientIgnoresInitialIdleBeforeFirstTurn();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
