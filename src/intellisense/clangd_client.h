#pragma once

#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "buffer.h"
#include "intellisense/completion.h"
#include "json.h"

namespace flowstate {

enum class CompletionEventKind {
    Completed,
    Error,
};

struct CompletionEvent {
    CompletionEventKind kind = CompletionEventKind::Completed;
    int request_id = 0;
    std::vector<CompletionItem> items;
    std::string error_message;
};

class ClangdClient {
  public:
    ClangdClient() = default;
    ~ClangdClient();

    bool Start(const std::filesystem::path& project_root, std::string* error);
    bool IsStarted() const;
    bool SyncDocument(const Buffer& buffer, std::string* error);
    std::optional<int> RequestCompletion(const Buffer& buffer, Cursor cursor, std::string* error);
    std::vector<CompletionEvent> PollEvents();
    void Shutdown();

  private:
    bool SendMessage(const std::string& json, std::string* error);
    bool SendRequest(int id, const std::string& method, JsonValue params, std::string* error);
    bool SendNotification(const std::string& method, JsonValue params, std::string* error);
    void ReadAvailableMessages();
    void HandleMessage(const JsonValue& message);
    void HandleCompletionResponse(int request_id, const JsonValue& result);
    void CheckProcessExit();
    bool SendDidOpen(const Buffer& buffer, const std::string& uri, std::string* error);
    bool SendDidChange(const Buffer& buffer, const std::string& uri, std::string* error);
    void SendDidClose();

    int input_fd_ = -1;
    int output_fd_ = -1;
    int child_pid_ = -1;
    int next_request_id_ = 1;
    int document_version_ = 0;
    std::string current_uri_;
    std::string read_buffer_;
    std::deque<CompletionEvent> queued_events_;
};

std::filesystem::path ResolveClangdProjectRoot(const Buffer& buffer);
std::string FileUriFromPath(const std::filesystem::path& path);
std::vector<CompletionItem> ParseCompletionItemsForTest(const JsonValue& result);

}  // namespace flowstate
