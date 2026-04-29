#include "ai/mock_client.h"

#include <fstream>
#include <sstream>

#include "diff.h"

namespace patchwork {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return "Fixture missing: " + path.string();
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

}  // namespace

MockAiClient::MockAiClient(std::filesystem::path fixture_root)
    : fixture_root_(std::move(fixture_root)) {}

AiResponse MockAiClient::Complete(const AiRequest& request) {
    const std::filesystem::path fixture = FixtureFor(request);
    const std::string raw_text = ReadTextFile(fixture);
    const std::string diff_text = ExtractDiffText(raw_text);

    AiResponse response;
    response.raw_text = raw_text;
    if (!diff_text.empty()) {
        response.diff_text = diff_text;
        response.kind =
            (diff_text == raw_text) ? AiResponseKind::DiffOnly : AiResponseKind::DiffWithExplanation;
    } else {
        response.kind = AiResponseKind::ExplanationOnly;
    }
    return response;
}

std::filesystem::path MockAiClient::FixtureFor(const AiRequest& request) const {
    switch (request.kind) {
        case AiRequestKind::Explain:
            return fixture_root_ / "ai_explain_response.txt";
        case AiRequestKind::Fix:
        case AiRequestKind::Refactor:
            return fixture_root_ / "simple_patch.diff";
        case AiRequestKind::ErrorExplain:
            return fixture_root_ / "ai_error_response.txt";
    }
    return fixture_root_ / "ai_explain_response.txt";
}

}  // namespace patchwork

