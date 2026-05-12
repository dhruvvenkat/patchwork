#include "ai/mock_client.h"

#include <fstream>
#include <sstream>

#include "diff.h"

namespace flowstate {

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

bool MockAiClient::StartRequest(const AiRequest& request, std::string* error) {
    if (!queued_events_.empty()) {
        if (error != nullptr) {
            *error = "Mock AI request already in progress.";
        }
        return false;
    }

    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Connecting});
    const AiResponse response = BuildResponse(request);
    if (response.kind == AiResponseKind::Error) {
        queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Failed});
        queued_events_.push_back({.kind = AiEventKind::Error, .state = AiRequestState::Failed, .error_message = response.error_message});
        return true;
    }

    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Streaming});
    if (!response.raw_text.empty()) {
        queued_events_.push_back({.kind = AiEventKind::TextDelta,
                                  .state = AiRequestState::Streaming,
                                  .text_delta = response.raw_text});
    }
    queued_events_.push_back({.kind = AiEventKind::Completed, .state = AiRequestState::Complete, .response = response});
    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Complete});
    return true;
}

std::vector<AiEvent> MockAiClient::PollEvents() {
    std::vector<AiEvent> events;
    events.reserve(queued_events_.size());
    while (!queued_events_.empty()) {
        events.push_back(std::move(queued_events_.front()));
        queued_events_.pop_front();
    }
    return events;
}

bool MockAiClient::HasActiveRequest() const { return !queued_events_.empty(); }

void MockAiClient::Shutdown() { queued_events_.clear(); }

AiResponse MockAiClient::BuildResponse(const AiRequest& request) const {
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

}  // namespace flowstate
