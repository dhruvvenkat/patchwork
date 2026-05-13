#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>

#include "ai/local_agent.h"
#include "json.h"

namespace flowstate {

class CodexAppServerClient : public ILocalAgentClient {
  public:
    explicit CodexAppServerClient(std::string profile);
    ~CodexAppServerClient() override;

    bool StartSession(const LocalAgentSessionConfig& config,
                      std::string* session_id,
                      std::string* error) override;
    bool SendMessage(const std::string& session_id,
                     const LocalAgentRequest& request,
                     std::string* error) override;
    std::vector<LocalAgentEvent> PollEvents() override;
    bool HasActiveMessage() const override;
    void CloseSession(const std::string& session_id) override;
    void Shutdown() override;

  private:
    struct PendingResponse {
        bool done = false;
        bool success = false;
        JsonValue payload;
        std::string error_message;
    };

    bool EnsureServerStarted(std::string* error);
    bool InitializeServer(std::string* error);
    bool SendRpcRequest(const std::string& method,
                        JsonValue params,
                        JsonValue* result,
                        std::string* error);
    bool WriteMessage(const std::string& message, std::string* error);
    void ReaderLoop();
    void StderrLoop();
    void HandleServerLine(const std::string& line);
    void HandleRpcResponse(const JsonValue& message);
    void HandleNotification(const JsonValue& message);
    void HandleDisconnect(const std::string& reason);
    void ResetThreadTrackingLocked();
    void EnqueueEventLocked(LocalAgentEvent event);
    void SetSessionStateLocked(const std::string& session_id, LocalAgentSessionState state);
    std::string BuildErrorMessageLocked(const std::string& fallback) const;
    void TerminateProcess();

    std::string profile_;
    mutable std::mutex mutex_;
    std::condition_variable response_cv_;
    std::deque<LocalAgentEvent> queued_events_;
    std::map<int, PendingResponse> pending_responses_;
    std::thread reader_thread_;
    std::thread stderr_thread_;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    int next_request_id_ = 1;
    pid_t child_pid_ = -1;
    bool server_started_ = false;
    bool initialized_ = false;
    bool shutdown_requested_ = false;
    bool disconnected_ = false;
    std::string stderr_output_;
    std::string current_session_id_;
    LocalAgentSessionState session_state_ = LocalAgentSessionState::Closed;
    std::string active_turn_id_;
    std::string active_message_id_;
    std::string final_message_text_;
};

}  // namespace flowstate
