#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ai/mock_client.h"
#include "build.h"
#include "buffer.h"
#include "command.h"
#include "diff.h"
#include "patch.h"
#include "selection.h"

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

void TestMockAiClient() {
    patchwork::MockAiClient client(std::filesystem::path(PATCHWORK_SOURCE_DIR) / "tests" / "fixtures");
    const patchwork::AiResponse explain = client.Complete({.kind = patchwork::AiRequestKind::Explain});
    const patchwork::AiResponse fix = client.Complete({.kind = patchwork::AiRequestKind::Fix});

    Expect(explain.kind == patchwork::AiResponseKind::ExplanationOnly, "explain fixture should be plain text");
    Expect(fix.diff_text.has_value(), "fix fixture should include a diff");
}

}  // namespace

int main() {
    try {
        TestBufferEditing();
        TestSelectionExtraction();
        TestCommandParsing();
        TestDiffParsingAndPatchApply();
        TestDiffExtractionWithProse();
        TestBuildRunner();
        TestMockAiClient();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
