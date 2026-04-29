#pragma once

#include <optional>
#include <string>

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

class IAiClient {
  public:
    virtual ~IAiClient() = default;
    virtual AiResponse Complete(const AiRequest& request) = 0;
};

}  // namespace patchwork

