#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ai/rate_limit.h"

namespace flowstate {

enum class LocalAgentEventKind {
    SessionStateChanged,
    TextDelta,
    FinalText,
    RateLimitsUpdated,
    Error,
};

enum class LocalAgentSessionState {
    Connecting,
    Idle,
    Active,
    Failed,
    Closed,
};

struct LocalAgentSessionConfig {
    std::filesystem::path project_root;
    std::string model;
};

struct LocalAgentRequest {
    std::string prompt;
    std::filesystem::path cwd;
};

struct LocalAgentEvent {
    LocalAgentEventKind kind = LocalAgentEventKind::SessionStateChanged;
    std::string session_id;
    LocalAgentSessionState session_state = LocalAgentSessionState::Connecting;
    std::string text_delta;
    std::string final_text;
    std::string error_message;
    RateLimitSnapshotInfo rate_limits;
};

class ILocalAgentClient {
  public:
    virtual ~ILocalAgentClient() = default;

    virtual bool StartSession(const LocalAgentSessionConfig& config,
                              std::string* session_id,
                              std::string* error) = 0;
    virtual bool SendMessage(const std::string& session_id,
                             const LocalAgentRequest& request,
                             std::string* error) = 0;
    virtual std::vector<LocalAgentEvent> PollEvents() = 0;
    virtual bool RefreshRateLimits(std::string* error) = 0;
    virtual bool HasActiveMessage() const = 0;
    virtual void CloseSession(const std::string& session_id) = 0;
    virtual void Shutdown() = 0;
};

}  // namespace flowstate
