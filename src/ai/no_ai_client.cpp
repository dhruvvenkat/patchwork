#include "ai/no_ai_client.h"

namespace flowstate {

bool NoAiClient::StartRequest(const AiRequest&, std::string* error) {
    if (error != nullptr) {
        *error = "AI is disabled for this session. Start with --ai openai or --ai codex to enable it.";
    }
    return false;
}

std::vector<AiEvent> NoAiClient::PollEvents() {
    return {};
}

bool NoAiClient::HasActiveRequest() const {
    return false;
}

void NoAiClient::Shutdown() {}

}  // namespace flowstate
