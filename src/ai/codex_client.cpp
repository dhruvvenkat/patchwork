#include "ai/codex_client.h"

#include <cstdlib>
#include <filesystem>

#include "ai/codex_app_server_client.h"
#include "ai/prompt.h"
#include "diff.h"

namespace patchwork {

namespace {

AiResponseKind ClassifyResponse(const std::string& raw_text, const std::string& diff_text) {
    if (diff_text.empty()) {
        return AiResponseKind::ExplanationOnly;
    }
    if (raw_text == diff_text) {
        return AiResponseKind::DiffOnly;
    }
    return AiResponseKind::DiffWithExplanation;
}

std::filesystem::path ResolveProjectRoot(const AiRequest& request) {
    if (request.file_path.empty()) {
        return std::filesystem::current_path();
    }

    std::error_code error;
    const std::filesystem::path absolute_path = std::filesystem::absolute(request.file_path, error);
    std::filesystem::path current =
        (!error && !absolute_path.empty()) ? absolute_path.parent_path() : std::filesystem::current_path();
    if (current.empty()) {
        current = std::filesystem::current_path();
    }

    while (!current.empty()) {
        if (std::filesystem::exists(current / ".git", error)) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return (!error && !absolute_path.empty() && !absolute_path.parent_path().empty()) ? absolute_path.parent_path()
                                                                                       : std::filesystem::current_path();
}

}  // namespace

CodexClient::CodexClient()
    : CodexClient(std::make_unique<CodexAppServerClient>(
          []() {
              const char* profile = std::getenv("PATCHWORK_CODEX_PROFILE");
              return (profile != nullptr && *profile != '\0') ? std::string(profile) : std::string();
          }())) {}

CodexClient::CodexClient(std::unique_ptr<ILocalAgentClient> local_agent)
    : local_agent_(std::move(local_agent)) {
    const char* model = std::getenv("PATCHWORK_CODEX_MODEL");
    const char* profile = std::getenv("PATCHWORK_CODEX_PROFILE");
    model_ = (model != nullptr && *model != '\0') ? model : "";
    profile_ = (profile != nullptr && *profile != '\0') ? profile : "";
}

bool CodexClient::StartRequest(const AiRequest& request, std::string* error) {
    if (request_active_) {
        if (error != nullptr) {
            *error = "Codex request already in progress.";
        }
        return false;
    }

    queued_events_.clear();
    streamed_text_.clear();
    final_text_.clear();
    streaming_state_emitted_ = false;

    const std::filesystem::path project_root = ResolveProjectRoot(request);
    if (session_id_.has_value() && session_root_ != project_root) {
        local_agent_->CloseSession(*session_id_);
        session_id_.reset();
        session_root_.clear();
    }

    if (!session_id_.has_value()) {
        std::string session_id;
        if (!local_agent_->StartSession({.project_root = project_root, .model = model_}, &session_id, error)) {
            session_id_.reset();
            session_root_.clear();
            return false;
        }
        session_id_ = std::move(session_id);
        session_root_ = project_root;
    }

    if (!local_agent_->SendMessage(*session_id_,
                                   {.prompt = BuildPrompt(request), .cwd = project_root},
                                   error)) {
        session_id_.reset();
        session_root_.clear();
        return false;
    }

    request_active_ = true;
    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Connecting});
    return true;
}

std::vector<AiEvent> CodexClient::PollEvents() {
    for (const LocalAgentEvent& event : local_agent_->PollEvents()) {
        switch (event.kind) {
            case LocalAgentEventKind::SessionStateChanged:
                if (!request_active_) {
                    break;
                }
                if (event.session_state == LocalAgentSessionState::Idle) {
                    FinalizeRequest();
                } else if (event.session_state == LocalAgentSessionState::Failed ||
                           event.session_state == LocalAgentSessionState::Closed) {
                    session_id_.reset();
                    session_root_.clear();
                    request_active_ = false;
                    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Failed});
                    queued_events_.push_back({.kind = AiEventKind::Error,
                                              .state = AiRequestState::Failed,
                                              .error_message = "codex app-server disconnected."});
                }
                break;
            case LocalAgentEventKind::TextDelta:
                if (!request_active_) {
                    break;
                }
                if (!streaming_state_emitted_) {
                    streaming_state_emitted_ = true;
                    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Streaming});
                }
                streamed_text_ += event.text_delta;
                queued_events_.push_back(
                    {.kind = AiEventKind::TextDelta, .state = AiRequestState::Streaming, .text_delta = event.text_delta});
                break;
            case LocalAgentEventKind::FinalText:
                final_text_ = event.final_text;
                if (!request_active_ || final_text_.empty() || streaming_state_emitted_) {
                    break;
                }
                streaming_state_emitted_ = true;
                streamed_text_ = final_text_;
                queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Streaming});
                queued_events_.push_back(
                    {.kind = AiEventKind::TextDelta, .state = AiRequestState::Streaming, .text_delta = final_text_});
                break;
            case LocalAgentEventKind::Error:
                session_id_.reset();
                session_root_.clear();
                request_active_ = false;
                queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Failed});
                queued_events_.push_back(
                    {.kind = AiEventKind::Error, .state = AiRequestState::Failed, .error_message = event.error_message});
                break;
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

bool CodexClient::HasActiveRequest() const { return request_active_; }

void CodexClient::Shutdown() {
    if (session_id_.has_value()) {
        local_agent_->CloseSession(*session_id_);
        session_id_.reset();
        session_root_.clear();
    }
    local_agent_->Shutdown();
    queued_events_.clear();
    streamed_text_.clear();
    final_text_.clear();
    request_active_ = false;
    streaming_state_emitted_ = false;
}

void CodexClient::FinalizeRequest() {
    if (!request_active_) {
        return;
    }

    request_active_ = false;
    const std::string final_text = !final_text_.empty() ? final_text_ : streamed_text_;
    if (final_text.empty()) {
        queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Failed});
        queued_events_.push_back(
            {.kind = AiEventKind::Error, .state = AiRequestState::Failed, .error_message = "Codex returned no text."});
        return;
    }

    const std::string diff_text = ExtractDiffText(final_text);
    AiResponse response;
    response.kind = ClassifyResponse(final_text, diff_text);
    response.raw_text = final_text;
    if (!diff_text.empty()) {
        response.diff_text = diff_text;
    }

    queued_events_.push_back({.kind = AiEventKind::Completed, .state = AiRequestState::Complete, .response = response});
    queued_events_.push_back({.kind = AiEventKind::StateChanged, .state = AiRequestState::Complete});
}

}  // namespace patchwork
