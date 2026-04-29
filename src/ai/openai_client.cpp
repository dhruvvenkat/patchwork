#include "ai/openai_client.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>

#include "ai/prompt.h"
#include "diff.h"

namespace patchwork {

namespace {

std::string JsonEscape(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

std::string ShellEscapeSingleQuoted(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() + 8);
    escaped.push_back('\'');
    for (char ch : input) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

std::string RunCommand(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }

    std::array<char, 512> buffer{};
    std::string output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output.append(buffer.data());
    }
    pclose(pipe);
    return output;
}

std::string JsonUnescape(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '\\' || index + 1 >= input.size()) {
            output.push_back(input[index]);
            continue;
        }

        ++index;
        switch (input[index]) {
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '"':
                output.push_back('"');
                break;
            default:
                output.push_back(input[index]);
                break;
        }
    }
    return output;
}

std::string ExtractTextFields(const std::string& json) {
    std::string merged;
    size_t search_from = 0;
    while (true) {
        const size_t key = json.find("\"text\":\"", search_from);
        if (key == std::string::npos) {
            break;
        }
        size_t index = key + 8;
        std::string value;
        bool escaped = false;
        for (; index < json.size(); ++index) {
            const char ch = json[index];
            if (!escaped && ch == '"') {
                break;
            }
            if (!escaped && ch == '\\') {
                escaped = true;
                value.push_back(ch);
                continue;
            }
            escaped = false;
            value.push_back(ch);
        }
        if (!merged.empty()) {
            merged.append("\n\n");
        }
        merged += JsonUnescape(value);
        search_from = index;
    }
    return merged;
}

AiResponseKind ClassifyResponse(const std::string& raw_text, const std::string& diff_text) {
    if (diff_text.empty()) {
        return AiResponseKind::ExplanationOnly;
    }
    if (raw_text == diff_text) {
        return AiResponseKind::DiffOnly;
    }
    return AiResponseKind::DiffWithExplanation;
}

}  // namespace

OpenAiClient::OpenAiClient() {
    const char* model = std::getenv("PATCHWORK_OPENAI_MODEL");
    model_ = (model != nullptr && *model != '\0') ? model : "gpt-4.1-mini";
}

bool OpenAiClient::StartRequest(const AiRequest& request, std::string* error) {
    if (request_active_) {
        if (error != nullptr) {
            *error = "OpenAI request already in progress.";
        }
        return false;
    }

    queued_events_.clear();
    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Connecting});
    pending_response_ =
        std::async(std::launch::async, [this, request]() { return CompleteRequest(request); });
    request_active_ = true;
    return true;
}

std::vector<AiEvent> OpenAiClient::PollEvents() {
    if (request_active_ && pending_response_.valid() &&
        pending_response_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const AiResponse response = pending_response_.get();
        request_active_ = false;

        if (response.kind == AiResponseKind::Error) {
            queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Failed});
            queued_events_.push_back(
                {.kind = AiEventKind::Error, .state = AiRequestState::Failed, .error_message = response.error_message});
        } else {
            queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Streaming});
            if (!response.raw_text.empty()) {
                queued_events_.push_back({.kind = AiEventKind::TextDelta,
                                          .state = AiRequestState::Streaming,
                                          .text_delta = response.raw_text});
            }
            queued_events_.push_back(
                {.kind = AiEventKind::Completed, .state = AiRequestState::Complete, .response = response});
            queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Complete});
        }
    }

    std::vector<AiEvent> events;
    events.reserve(queued_events_.size());
    while (!queued_events_.empty()) {
        events.push_back(std::move(queued_events_.front()));
        queued_events_.pop_front();
    }
    return events;
}

bool OpenAiClient::HasActiveRequest() const { return request_active_; }

void OpenAiClient::Shutdown() {
    if (request_active_ && pending_response_.valid()) {
        pending_response_.wait();
        request_active_ = false;
    }
    queued_events_.clear();
}

AiResponse OpenAiClient::CompleteRequest(const AiRequest& request) {
    const char* api_key = std::getenv("PATCHWORK_OPENAI_API_KEY");
    if (api_key == nullptr || *api_key == '\0') {
        return {.kind = AiResponseKind::Error, .error_message = "PATCHWORK_OPENAI_API_KEY is not set."};
    }

    const char* base_url = std::getenv("PATCHWORK_OPENAI_BASE_URL");
    const std::string endpoint =
        (base_url != nullptr && *base_url != '\0') ? base_url : "https://api.openai.com/v1/responses";

    const std::string prompt = BuildPrompt(request);
    const std::string payload = "{\"model\":\"" + JsonEscape(model_) + "\",\"input\":\"" +
                                JsonEscape(prompt) + "\"}";

    std::string command = "curl -sS " + ShellEscapeSingleQuoted(endpoint) + " -H " +
                          ShellEscapeSingleQuoted("Content-Type: application/json") + " -H " +
                          ShellEscapeSingleQuoted(std::string("Authorization: Bearer ") + api_key) + " -d " +
                          ShellEscapeSingleQuoted(payload);

    const std::string response_body = RunCommand(command);
    if (response_body.empty()) {
        return {.kind = AiResponseKind::Error, .error_message = "No response from OpenAI client."};
    }

    std::string raw_text = ExtractTextFields(response_body);
    if (raw_text.empty()) {
        raw_text = response_body;
    }

    const std::string diff_text = ExtractDiffText(raw_text);
    AiResponse response;
    response.kind = ClassifyResponse(raw_text, diff_text);
    response.raw_text = raw_text;
    if (!diff_text.empty()) {
        response.diff_text = diff_text;
    }
    return response;
}

}  // namespace patchwork
