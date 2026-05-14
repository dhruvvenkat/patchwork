#include "ai/codex_app_server_client.h"

#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <utility>
#include <unistd.h>

namespace flowstate {

namespace {

constexpr int kResponseTimeoutSeconds = 10;

bool WriteAll(int fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = ::write(fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string TrimLine(std::string line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

const JsonValue* JsonAtPath(const JsonValue& root, std::initializer_list<std::string_view> path) {
    const JsonValue* current = &root;
    for (const std::string_view part : path) {
        current = current->find(part);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

std::optional<std::string> JsonStringAtPath(const JsonValue& root,
                                            std::initializer_list<std::string_view> path) {
    const JsonValue* value = JsonAtPath(root, path);
    if (value == nullptr || !value->isString()) {
        return std::nullopt;
    }
    return value->stringValue();
}

std::optional<int> JsonIntAtPath(const JsonValue& root, std::initializer_list<std::string_view> path) {
    const JsonValue* value = JsonAtPath(root, path);
    if (value == nullptr || !value->isNumber()) {
        return std::nullopt;
    }
    return value->intValue();
}

std::optional<double> JsonNumberAtPath(const JsonValue& root,
                                       std::initializer_list<std::string_view> path) {
    const JsonValue* value = JsonAtPath(root, path);
    if (value == nullptr || !value->isNumber()) {
        return std::nullopt;
    }
    return value->numberValue();
}

std::string JsonRpcRequest(int id, const std::string& method, const JsonValue* params) {
    JsonValue::Object payload;
    payload["jsonrpc"] = JsonValue("2.0");
    payload["id"] = JsonValue(id);
    payload["method"] = JsonValue(method);
    if (params != nullptr) {
        payload["params"] = *params;
    }
    return JsonValue(std::move(payload)).Serialize();
}

JsonValue CreateInitializeParams() {
    JsonValue::Object client_info;
    client_info["name"] = JsonValue("flowstate");
    client_info["version"] = JsonValue("0.1");

    JsonValue::Object params;
    params["clientInfo"] = JsonValue(std::move(client_info));
    params["capabilities"] = JsonValue();
    return JsonValue(std::move(params));
}

JsonValue CreateThreadStartParams(const LocalAgentSessionConfig& config) {
    JsonValue::Object params;
    params["cwd"] = JsonValue(config.project_root.string());
    params["approvalPolicy"] = JsonValue("never");
    params["sandbox"] = JsonValue("read-only");
    params["ephemeral"] = JsonValue(true);
    params["experimentalRawEvents"] = JsonValue(false);
    params["persistExtendedHistory"] = JsonValue(false);
    params["serviceName"] = JsonValue("flowstate");
    if (!config.model.empty()) {
        params["model"] = JsonValue(config.model);
    }
    return JsonValue(std::move(params));
}

JsonValue CreateTurnStartParams(const std::string& thread_id, const LocalAgentRequest& request) {
    JsonValue::Object input_item;
    input_item["type"] = JsonValue("text");
    input_item["text"] = JsonValue(request.prompt);
    input_item["text_elements"] = JsonValue(JsonValue::Array{});

    JsonValue::Object params;
    params["threadId"] = JsonValue(thread_id);
    params["input"] = JsonValue(JsonValue::Array{JsonValue(std::move(input_item))});
    params["cwd"] = JsonValue(request.cwd.string());
    return JsonValue(std::move(params));
}

std::string ErrorMessageFromPayload(const JsonValue& payload, const std::string& fallback) {
    if (const std::optional<std::string> message = JsonStringAtPath(payload, {"message"}); message.has_value()) {
        return *message;
    }
    if (const std::optional<std::string> message =
            JsonStringAtPath(payload, {"error", "message"});
        message.has_value()) {
        return *message;
    }
    return fallback;
}

std::optional<int64_t> JsonInt64AtPath(const JsonValue& root,
                                       std::initializer_list<std::string_view> path) {
    const std::optional<double> value = JsonNumberAtPath(root, path);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<int64_t>(*value);
}

RateLimitWindowInfo ParseRateLimitWindow(const JsonValue* value) {
    RateLimitWindowInfo window;
    if (value == nullptr || !value->isObject()) {
        return window;
    }

    const std::optional<double> used_percent = JsonNumberAtPath(*value, {"usedPercent"});
    if (!used_percent.has_value()) {
        return window;
    }

    window.available = true;
    window.used_percent = *used_percent;
    window.window_duration_mins = JsonInt64AtPath(*value, {"windowDurationMins"});
    window.resets_at = JsonInt64AtPath(*value, {"resetsAt"});
    return window;
}

RateLimitSnapshotInfo ParseRateLimitSnapshot(const JsonValue& value) {
    RateLimitSnapshotInfo snapshot;
    if (!value.isObject()) {
        return snapshot;
    }

    snapshot.available = true;
    if (const std::optional<std::string> limit_id = JsonStringAtPath(value, {"limitId"});
        limit_id.has_value()) {
        snapshot.limit_id = *limit_id;
    }
    if (const std::optional<std::string> limit_name = JsonStringAtPath(value, {"limitName"});
        limit_name.has_value()) {
        snapshot.limit_name = *limit_name;
    }
    snapshot.primary = ParseRateLimitWindow(value.find("primary"));
    snapshot.secondary = ParseRateLimitWindow(value.find("secondary"));
    snapshot.available = snapshot.primary.available || snapshot.secondary.available;
    return snapshot;
}

RateLimitSnapshotInfo ParseRateLimitsResponse(const JsonValue& result) {
    const JsonValue* by_limit_id = result.find("rateLimitsByLimitId");
    if (by_limit_id != nullptr && by_limit_id->isObject()) {
        if (const JsonValue* codex = by_limit_id->find("codex"); codex != nullptr) {
            const RateLimitSnapshotInfo snapshot = ParseRateLimitSnapshot(*codex);
            if (snapshot.available) {
                return snapshot;
            }
        }

        RateLimitSnapshotInfo first_available;
        for (const auto& [limit_id, value] : by_limit_id->objectValue()) {
            RateLimitSnapshotInfo snapshot = ParseRateLimitSnapshot(value);
            if (!snapshot.available) {
                continue;
            }
            if (snapshot.limit_id == "codex" || limit_id == "codex") {
                return snapshot;
            }
            if (!first_available.available) {
                first_available = std::move(snapshot);
            }
        }
        if (first_available.available) {
            return first_available;
        }
    }

    if (const JsonValue* rate_limits = result.find("rateLimits"); rate_limits != nullptr) {
        return ParseRateLimitSnapshot(*rate_limits);
    }

    return {};
}

}  // namespace

CodexAppServerClient::CodexAppServerClient(std::string profile) : profile_(std::move(profile)) {}

CodexAppServerClient::~CodexAppServerClient() { Shutdown(); }

bool CodexAppServerClient::StartSession(const LocalAgentSessionConfig& config,
                                        std::string* session_id,
                                        std::string* error) {
    if (!EnsureServerStarted(error)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!current_session_id_.empty()) {
            if (session_id != nullptr) {
                *session_id = current_session_id_;
            }
            return true;
        }
        SetSessionStateLocked("", LocalAgentSessionState::Connecting);
    }

    JsonValue result;
    if (!SendRpcRequest("thread/start", CreateThreadStartParams(config), &result, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_state_ = LocalAgentSessionState::Failed;
        return false;
    }

    const std::optional<std::string> started_session =
        JsonStringAtPath(result, {"thread", "id"});
    if (!started_session.has_value()) {
        if (error != nullptr) {
            *error = "codex app-server returned a thread/start response without a thread id.";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_session_id_ = *started_session;
        ResetThreadTrackingLocked();
        SetSessionStateLocked(current_session_id_, LocalAgentSessionState::Idle);
    }

    if (session_id != nullptr) {
        *session_id = *started_session;
    }
    return true;
}

bool CodexAppServerClient::SendMessage(const std::string& session_id,
                                       const LocalAgentRequest& request,
                                       std::string* error) {
    if (!EnsureServerStarted(error)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_id.empty() || session_id != current_session_id_) {
            if (error != nullptr) {
                *error = "codex app-server session is not available.";
            }
            return false;
        }
        if (!active_turn_id_.empty()) {
            if (error != nullptr) {
                *error = "codex app-server already has an active AI request.";
            }
            return false;
        }
        ResetThreadTrackingLocked();
    }

    JsonValue result;
    if (!SendRpcRequest("turn/start", CreateTurnStartParams(session_id, request), &result, error)) {
        return false;
    }

    const std::optional<std::string> turn_id = JsonStringAtPath(result, {"turn", "id"});
    if (!turn_id.has_value()) {
        if (error != nullptr) {
            *error = "codex app-server returned a turn/start response without a turn id.";
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn_id_ = *turn_id;
        final_message_text_.clear();
        SetSessionStateLocked(current_session_id_, LocalAgentSessionState::Active);
    }
    return true;
}

std::vector<LocalAgentEvent> CodexAppServerClient::PollEvents() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LocalAgentEvent> events;
    events.reserve(queued_events_.size());
    while (!queued_events_.empty()) {
        events.push_back(std::move(queued_events_.front()));
        queued_events_.pop_front();
    }
    return events;
}

bool CodexAppServerClient::RefreshRateLimits(std::string* error) {
    if (!EnsureServerStarted(error)) {
        return false;
    }

    JsonValue result;
    if (!SendRpcRequestWithoutParams("account/rateLimits/read", &result, error)) {
        return false;
    }

    RateLimitSnapshotInfo snapshot = ParseRateLimitsResponse(result);
    if (!snapshot.available) {
        if (error != nullptr) {
            *error = "codex app-server returned no rate limit data.";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    EnqueueEventLocked(LocalAgentEvent{
        .kind = LocalAgentEventKind::RateLimitsUpdated,
        .session_state = session_state_,
        .rate_limits = std::move(snapshot),
    });
    return true;
}

bool CodexAppServerClient::HasActiveMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_turn_id_.empty();
}

void CodexAppServerClient::CloseSession(const std::string& session_id) {
    if (session_id.empty()) {
        return;
    }

    std::string ignore_error;
    if (EnsureServerStarted(&ignore_error)) {
        JsonValue result;
        JsonValue::Object params;
        params["threadId"] = JsonValue(session_id);
        SendRpcRequest("thread/unsubscribe", JsonValue(std::move(params)), &result, &ignore_error);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (session_id == current_session_id_) {
        current_session_id_.clear();
        ResetThreadTrackingLocked();
        SetSessionStateLocked(session_id, LocalAgentSessionState::Closed);
    }
}

void CodexAppServerClient::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return;
        }
        shutdown_requested_ = true;
    }

    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id = current_session_id_;
    }
    if (!session_id.empty()) {
        CloseSession(session_id);
    }

    TerminateProcess();
}

bool CodexAppServerClient::EnsureServerStarted(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_started_ && !disconnected_) {
            return true;
        }
        if (shutdown_requested_) {
            if (error != nullptr) {
                *error = "codex app-server has already been shut down.";
            }
            return false;
        }
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        if (stdin_pipe[0] != -1) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] != -1) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        if (stderr_pipe[0] != -1) {
            close(stderr_pipe[0]);
            close(stderr_pipe[1]);
        }
        if (error != nullptr) {
            *error = "Failed to create pipes for codex app-server.";
        }
        return false;
    }

    const pid_t pid = fork();
    if (pid == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        if (error != nullptr) {
            *error = "Failed to fork codex app-server.";
        }
        return false;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        if (!profile_.empty()) {
            execlp("codex",
                   "codex",
                   "--profile",
                   profile_.c_str(),
                   "app-server",
                   "--listen",
                   "stdio://",
                   static_cast<char*>(nullptr));
        } else {
            execlp("codex",
                   "codex",
                   "app-server",
                   "--listen",
                   "stdio://",
                   static_cast<char*>(nullptr));
        }
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        stderr_fd_ = stderr_pipe[0];
        child_pid_ = pid;
        server_started_ = true;
        disconnected_ = false;
        initialized_ = false;
        stderr_output_.clear();
    }

    reader_thread_ = std::thread(&CodexAppServerClient::ReaderLoop, this);
    stderr_thread_ = std::thread(&CodexAppServerClient::StderrLoop, this);

    if (!InitializeServer(error)) {
        TerminateProcess();
        return false;
    }
    return true;
}

bool CodexAppServerClient::InitializeServer(std::string* error) {
    JsonValue result;
    if (!SendRpcRequest("initialize", CreateInitializeParams(), &result, error)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return true;
}

bool CodexAppServerClient::SendRpcRequest(const std::string& method,
                                          JsonValue params,
                                          JsonValue* result,
                                          std::string* error) {
    return SendRpcRequestWithOptionalParams(method, &params, result, error);
}

bool CodexAppServerClient::SendRpcRequestWithoutParams(const std::string& method,
                                                       JsonValue* result,
                                                       std::string* error) {
    return SendRpcRequestWithOptionalParams(method, nullptr, result, error);
}

bool CodexAppServerClient::SendRpcRequestWithOptionalParams(const std::string& method,
                                                            const JsonValue* params,
                                                            JsonValue* result,
                                                            std::string* error) {
    int request_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (disconnected_) {
            if (error != nullptr) {
                *error = BuildErrorMessageLocked("codex app-server is disconnected.");
            }
            return false;
        }
        request_id = next_request_id_++;
        pending_responses_.emplace(request_id, PendingResponse{});
    }

    if (!WriteMessage(JsonRpcRequest(request_id, method, params) + "\n", error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_responses_.erase(request_id);
        return false;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = response_cv_.wait_for(lock,
                                             std::chrono::seconds(kResponseTimeoutSeconds),
                                             [this, request_id]() {
                                                 const auto found = pending_responses_.find(request_id);
                                                 return disconnected_ ||
                                                        (found != pending_responses_.end() && found->second.done);
                                             });
    if (!ready) {
        pending_responses_.erase(request_id);
        if (error != nullptr) {
            *error = "Timed out waiting for codex app-server to respond to " + method + ".";
        }
        return false;
    }
    if (disconnected_) {
        pending_responses_.erase(request_id);
        if (error != nullptr) {
            *error = BuildErrorMessageLocked("codex app-server disconnected.");
        }
        return false;
    }

    const auto found = pending_responses_.find(request_id);
    if (found == pending_responses_.end()) {
        if (error != nullptr) {
            *error = "codex app-server lost a pending response for " + method + ".";
        }
        return false;
    }

    const PendingResponse response = found->second;
    pending_responses_.erase(found);
    if (!response.success) {
        if (error != nullptr) {
            *error = response.error_message.empty() ? ("codex app-server rejected " + method + ".")
                                                    : response.error_message;
        }
        return false;
    }

    if (result != nullptr) {
        *result = response.payload;
    }
    return true;
}

bool CodexAppServerClient::WriteMessage(const std::string& message, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stdin_fd_ == -1 || disconnected_) {
        if (error != nullptr) {
            *error = BuildErrorMessageLocked("codex app-server stdin is unavailable.");
        }
        return false;
    }
    if (!WriteAll(stdin_fd_, message)) {
        if (error != nullptr) {
            *error = BuildErrorMessageLocked("Failed to write to codex app-server.");
        }
        return false;
    }
    return true;
}

void CodexAppServerClient::ReaderLoop() {
    FILE* stream = fdopen(stdout_fd_, "r");
    if (stream == nullptr) {
        HandleDisconnect("Failed to read codex app-server output.");
        return;
    }

    char* line = nullptr;
    size_t capacity = 0;
    while (getline(&line, &capacity, stream) != -1) {
        HandleServerLine(TrimLine(line));
    }

    free(line);
    fclose(stream);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stdout_fd_ = -1;
    }
    HandleDisconnect("codex app-server disconnected.");
}

void CodexAppServerClient::StderrLoop() {
    FILE* stream = fdopen(stderr_fd_, "r");
    if (stream == nullptr) {
        return;
    }

    char* line = nullptr;
    size_t capacity = 0;
    while (getline(&line, &capacity, stream) != -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string trimmed = TrimLine(line);
        if (!trimmed.empty()) {
            if (!stderr_output_.empty()) {
                stderr_output_ += '\n';
            }
            stderr_output_ += trimmed;
        }
    }

    free(line);
    fclose(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    stderr_fd_ = -1;
}

void CodexAppServerClient::HandleServerLine(const std::string& line) {
    if (line.empty()) {
        return;
    }

    std::string parse_error;
    const std::optional<JsonValue> message = JsonValue::Parse(line, &parse_error);
    if (!message.has_value() || !message->isObject()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stderr_output_.empty()) {
            stderr_output_ += '\n';
        }
        stderr_output_ += line;
        return;
    }

    if (message->find("id") != nullptr && (message->find("result") != nullptr || message->find("error") != nullptr)) {
        HandleRpcResponse(*message);
        return;
    }
    if (message->find("method") != nullptr) {
        HandleNotification(*message);
    }
}

void CodexAppServerClient::HandleRpcResponse(const JsonValue& message) {
    const std::optional<int> response_id = JsonIntAtPath(message, {"id"});
    if (!response_id.has_value()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto found = pending_responses_.find(*response_id);
    if (found == pending_responses_.end()) {
        return;
    }

    found->second.done = true;
    if (const JsonValue* result = message.find("result"); result != nullptr) {
        found->second.success = true;
        found->second.payload = *result;
    } else if (const JsonValue* error = message.find("error"); error != nullptr) {
        found->second.success = false;
        found->second.error_message = ErrorMessageFromPayload(*error, "codex app-server returned an error.");
    } else {
        found->second.success = false;
        found->second.error_message = "codex app-server returned an invalid response.";
    }
    response_cv_.notify_all();
}

void CodexAppServerClient::HandleNotification(const JsonValue& message) {
    const std::optional<std::string> method = JsonStringAtPath(message, {"method"});
    if (!method.has_value()) {
        return;
    }

    const JsonValue* params = message.find("params");
    if (params == nullptr || !params->isObject()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (*method == "account/rateLimits/updated") {
        const JsonValue* rate_limits = params->find("rateLimits");
        if (rate_limits == nullptr) {
            return;
        }
        RateLimitSnapshotInfo snapshot = ParseRateLimitSnapshot(*rate_limits);
        if (snapshot.available) {
            EnqueueEventLocked(LocalAgentEvent{
                .kind = LocalAgentEventKind::RateLimitsUpdated,
                .session_state = session_state_,
                .rate_limits = std::move(snapshot),
            });
        }
        return;
    }

    if (*method == "thread/status/changed") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        const std::optional<std::string> status_type = JsonStringAtPath(*params, {"status", "type"});
        if (thread_id.has_value() && thread_id == current_session_id_ && status_type.has_value()) {
            if (*status_type == "active") {
                SetSessionStateLocked(*thread_id, LocalAgentSessionState::Active);
            } else if (*status_type == "idle") {
                SetSessionStateLocked(*thread_id, LocalAgentSessionState::Idle);
            } else if (*status_type == "systemError") {
                SetSessionStateLocked(*thread_id, LocalAgentSessionState::Failed);
            }
        }
        return;
    }

    if (*method == "item/agentMessage/delta") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        const std::optional<std::string> turn_id = JsonStringAtPath(*params, {"turnId"});
        const std::optional<std::string> item_id = JsonStringAtPath(*params, {"itemId"});
        const std::optional<std::string> delta = JsonStringAtPath(*params, {"delta"});
        if (thread_id.has_value() && turn_id.has_value() && delta.has_value() && thread_id == current_session_id_ &&
            (active_turn_id_.empty() || *turn_id == active_turn_id_)) {
            if (item_id.has_value()) {
                active_message_id_ = *item_id;
            }
            EnqueueEventLocked(LocalAgentEvent{
                .kind = LocalAgentEventKind::TextDelta,
                .session_id = *thread_id,
                .session_state = session_state_,
                .text_delta = *delta,
            });
        }
        return;
    }

    if (*method == "item/completed") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        const std::optional<std::string> turn_id = JsonStringAtPath(*params, {"turnId"});
        const std::optional<std::string> item_type = JsonStringAtPath(*params, {"item", "type"});
        if (thread_id.has_value() && turn_id.has_value() && item_type.has_value() &&
            thread_id == current_session_id_ && (active_turn_id_.empty() || *turn_id == active_turn_id_) &&
            *item_type == "agentMessage") {
            if (const std::optional<std::string> item_id = JsonStringAtPath(*params, {"item", "id"});
                item_id.has_value()) {
                active_message_id_ = *item_id;
            }
            if (const std::optional<std::string> text = JsonStringAtPath(*params, {"item", "text"});
                text.has_value()) {
                final_message_text_ = *text;
                EnqueueEventLocked(LocalAgentEvent{
                    .kind = LocalAgentEventKind::FinalText,
                    .session_id = *thread_id,
                    .session_state = session_state_,
                    .final_text = *text,
                });
            }
        }
        return;
    }

    if (*method == "turn/completed") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        const std::optional<std::string> turn_id = JsonStringAtPath(*params, {"turn", "id"});
        const std::optional<std::string> status = JsonStringAtPath(*params, {"turn", "status"});
        if (!thread_id.has_value() || !turn_id.has_value() || !status.has_value() || thread_id != current_session_id_ ||
            (!active_turn_id_.empty() && *turn_id != active_turn_id_)) {
            return;
        }

        if (*status == "completed") {
            active_turn_id_.clear();
            SetSessionStateLocked(*thread_id, LocalAgentSessionState::Idle);
            return;
        }

        std::string message_text = "codex app-server failed to complete the request.";
        if (const std::optional<std::string> turn_error =
                JsonStringAtPath(*params, {"turn", "error", "message"});
            turn_error.has_value()) {
            message_text = *turn_error;
        }
        active_turn_id_.clear();
        EnqueueEventLocked(LocalAgentEvent{
            .kind = LocalAgentEventKind::Error,
            .session_id = *thread_id,
            .session_state = LocalAgentSessionState::Failed,
            .error_message = message_text,
        });
        SetSessionStateLocked(*thread_id, LocalAgentSessionState::Failed);
        return;
    }

    if (*method == "error") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        if (!thread_id.has_value() || thread_id != current_session_id_) {
            return;
        }
        const std::string message_text =
            ErrorMessageFromPayload(*params, "codex app-server failed to process the request.");
        EnqueueEventLocked(LocalAgentEvent{
            .kind = LocalAgentEventKind::Error,
            .session_id = *thread_id,
            .session_state = LocalAgentSessionState::Failed,
            .error_message = message_text,
        });
        SetSessionStateLocked(*thread_id, LocalAgentSessionState::Failed);
        active_turn_id_.clear();
        return;
    }

    if (*method == "thread/closed") {
        const std::optional<std::string> thread_id = JsonStringAtPath(*params, {"threadId"});
        if (thread_id.has_value() && thread_id == current_session_id_) {
            SetSessionStateLocked(*thread_id, LocalAgentSessionState::Closed);
            current_session_id_.clear();
            ResetThreadTrackingLocked();
        }
    }
}

void CodexAppServerClient::HandleDisconnect(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disconnected_) {
        return;
    }

    disconnected_ = true;
    initialized_ = false;
    server_started_ = false;

    const std::string message = BuildErrorMessageLocked(reason);
    for (auto& [id, pending] : pending_responses_) {
        pending.done = true;
        pending.success = false;
        pending.error_message = message;
    }
    response_cv_.notify_all();

    if (!shutdown_requested_ && !active_turn_id_.empty()) {
        EnqueueEventLocked(LocalAgentEvent{
            .kind = LocalAgentEventKind::Error,
            .session_id = current_session_id_,
            .session_state = LocalAgentSessionState::Failed,
            .error_message = message,
        });
        SetSessionStateLocked(current_session_id_, LocalAgentSessionState::Failed);
        active_turn_id_.clear();
    }
}

void CodexAppServerClient::ResetThreadTrackingLocked() {
    active_turn_id_.clear();
    active_message_id_.clear();
    final_message_text_.clear();
}

void CodexAppServerClient::EnqueueEventLocked(LocalAgentEvent event) {
    queued_events_.push_back(std::move(event));
}

void CodexAppServerClient::SetSessionStateLocked(const std::string& session_id, LocalAgentSessionState state) {
    if (session_state_ == state && current_session_id_ == session_id) {
        return;
    }
    session_state_ = state;
    if (!session_id.empty()) {
        current_session_id_ = session_id;
    }
    EnqueueEventLocked(LocalAgentEvent{
        .kind = LocalAgentEventKind::SessionStateChanged,
        .session_id = current_session_id_,
        .session_state = state,
    });
}

std::string CodexAppServerClient::BuildErrorMessageLocked(const std::string& fallback) const {
    if (stderr_output_.empty()) {
        return fallback;
    }
    return fallback + "\n" + stderr_output_;
}

void CodexAppServerClient::TerminateProcess() {
    int stdin_fd = -1;
    pid_t child_pid = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stdin_fd = stdin_fd_;
        stdin_fd_ = -1;
        child_pid = child_pid_;
    }

    if (stdin_fd != -1) {
        close(stdin_fd);
    }

    if (child_pid > 0) {
        int status = 0;
        bool exited = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            const pid_t result = waitpid(child_pid, &status, WNOHANG);
            if (result == child_pid) {
                exited = true;
                break;
            }
            usleep(20000);
        }
        if (!exited) {
            kill(child_pid, SIGTERM);
            for (int attempt = 0; attempt < 10; ++attempt) {
                const pid_t result = waitpid(child_pid, &status, WNOHANG);
                if (result == child_pid) {
                    exited = true;
                    break;
                }
                usleep(20000);
            }
        }
        if (!exited) {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, &status, 0);
        }
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (stderr_thread_.joinable()) {
        stderr_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    child_pid_ = -1;
    stdout_fd_ = -1;
    stderr_fd_ = -1;
    stdin_fd_ = -1;
    server_started_ = false;
    initialized_ = false;
    disconnected_ = true;
    current_session_id_.clear();
    ResetThreadTrackingLocked();
    session_state_ = LocalAgentSessionState::Closed;
}

}  // namespace flowstate
