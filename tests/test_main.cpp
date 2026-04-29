#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "ai/mock_client.h"
#include "build.h"
#include "buffer.h"
#include "command.h"
#include "diff.h"
#include "json.h"
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
        TestJsonParsing();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
