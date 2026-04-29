#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buffer.h"

namespace patchwork {

enum class AiRequestKind {
    Explain,
    Fix,
    Refactor,
    ErrorExplain,
};

enum class AiResponseKind {
    ExplanationOnly,
    DiffOnly,
    DiffWithExplanation,
    Error,
};

struct AiRequest {
    AiRequestKind kind = AiRequestKind::Explain;
    std::string file_path;
    std::string language;
    Cursor cursor;
    std::string selected_text;
    std::string context_before;
    std::string context_after;
    std::string user_instruction;
    std::string build_command;
    std::string build_output;
};

struct AiResponse {
    AiResponseKind kind = AiResponseKind::ExplanationOnly;
    std::string raw_text;
    std::optional<std::string> diff_text;
    std::string error_message;
};

enum class AiEventKind {
    StateChanged,
    TextDelta,
    Completed,
    Error,
};

enum class AiRequestState {
    Connecting,
    Streaming,
    ParsingPatch,
    Failed,
    Complete,
};

struct AiEvent {
    AiEventKind kind = AiEventKind::StateChanged;
    AiRequestState state = AiRequestState::Connecting;
    std::string text_delta;
    AiResponse response;
    std::string error_message;
};

class IAiClient {
  public:
    virtual ~IAiClient() = default;
    virtual bool StartRequest(const AiRequest& request, std::string* error) = 0;
    virtual std::vector<AiEvent> PollEvents() = 0;
    virtual bool HasActiveRequest() const = 0;
    virtual void Shutdown() = 0;
};

}  // namespace patchwork
