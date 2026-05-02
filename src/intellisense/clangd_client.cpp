#include "intellisense/clangd_client.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace patchwork {

namespace {

bool IsExecutable(const std::filesystem::path& path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> FindExecutable(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path requested(name);
    if (requested.has_parent_path()) {
        return IsExecutable(requested) ? std::optional<std::filesystem::path>(requested) : std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }

    std::string_view paths(path_env);
    size_t start = 0;
    while (start <= paths.size()) {
        const size_t end = paths.find(':', start);
        const std::string_view entry =
            end == std::string_view::npos ? paths.substr(start) : paths.substr(start, end - start);
        const std::filesystem::path candidate =
            (entry.empty() ? std::filesystem::current_path() : std::filesystem::path(entry)) / std::string(name);
        if (IsExecutable(candidate)) {
            return candidate;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveClangdExecutable() {
    const char* configured = std::getenv("PATCHWORK_CLANGD_PATH");
    if (configured != nullptr && *configured != '\0') {
        return FindExecutable(configured);
    }
    return FindExecutable("clangd");
}

JsonValue::Object TextDocumentIdentifier(const std::string& uri) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    return text_document;
}

JsonValue::Object PositionObject(Cursor cursor) {
    JsonValue::Object position;
    position["line"] = JsonValue(static_cast<int>(cursor.row));
    position["character"] = JsonValue(static_cast<int>(cursor.col));
    return position;
}

JsonValue::Object RangeObject(Cursor start, Cursor end) {
    JsonValue::Object range;
    range["start"] = JsonValue(PositionObject(start));
    range["end"] = JsonValue(PositionObject(end));
    return range;
}

std::optional<Cursor> CursorFromJson(const JsonValue* value) {
    if (value == nullptr || !value->isObject()) {
        return std::nullopt;
    }
    const JsonValue* line = value->find("line");
    const JsonValue* character = value->find("character");
    if (line == nullptr || character == nullptr || !line->isNumber() || !character->isNumber() ||
        line->intValue() < 0 || character->intValue() < 0) {
        return std::nullopt;
    }
    return Cursor{static_cast<size_t>(line->intValue()), static_cast<size_t>(character->intValue())};
}

std::optional<CompletionTextEdit> TextEditFromJson(const JsonValue* value, bool snippet_format) {
    if (value == nullptr || !value->isObject()) {
        return std::nullopt;
    }

    const JsonValue* range = value->find("range");
    const JsonValue* new_text = value->find("newText");
    if (range == nullptr || !range->isObject() || new_text == nullptr || !new_text->isString()) {
        return std::nullopt;
    }

    const std::optional<Cursor> start = CursorFromJson(range->find("start"));
    const std::optional<Cursor> end = CursorFromJson(range->find("end"));
    if (!start.has_value() || !end.has_value()) {
        return std::nullopt;
    }

    return CompletionTextEdit{
        .start = *start,
        .end = *end,
        .new_text = snippet_format ? "" : new_text->stringValue(),
    };
}

std::string StringField(const JsonValue& object, std::string_view key) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->isString()) {
        return {};
    }
    return value->stringValue();
}

int IntField(const JsonValue& object, std::string_view key, int fallback = 0) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->isNumber()) {
        return fallback;
    }
    return value->intValue();
}

std::vector<CompletionItem> ParseCompletionItems(const JsonValue& result) {
    const JsonValue::Array* items = nullptr;
    if (result.isArray()) {
        items = &result.arrayValue();
    } else if (result.isObject()) {
        const JsonValue* item_value = result.find("items");
        if (item_value != nullptr && item_value->isArray()) {
            items = &item_value->arrayValue();
        }
    }
    if (items == nullptr) {
        return {};
    }

    std::vector<CompletionItem> parsed;
    parsed.reserve(items->size());
    for (const JsonValue& value : *items) {
        if (!value.isObject()) {
            continue;
        }
        const std::string label = StringField(value, "label");
        if (label.empty()) {
            continue;
        }

        const bool snippet_format = IntField(value, "insertTextFormat") == 2;
        CompletionItem item;
        item.label = label;
        item.detail = StringField(value, "detail");
        item.insert_text = snippet_format ? label : StringField(value, "insertText");
        if (item.insert_text.empty()) {
            item.insert_text = label;
        }

        if (const JsonValue* text_edit = value.find("textEdit"); text_edit != nullptr) {
            item.text_edit = TextEditFromJson(text_edit, snippet_format);
            if (item.text_edit.has_value() && item.text_edit->new_text.empty()) {
                item.text_edit->new_text = label;
            }
        }
        parsed.push_back(std::move(item));
        if (parsed.size() >= 100) {
            break;
        }
    }
    return parsed;
}

std::string HeaderForBody(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
}

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

std::optional<size_t> ParseContentLength(std::string_view header) {
    constexpr std::string_view kPrefix = "Content-Length:";
    size_t line_start = 0;
    while (line_start < header.size()) {
        const size_t line_end = header.find("\r\n", line_start);
        const std::string_view line =
            line_end == std::string_view::npos ? header.substr(line_start) : header.substr(line_start, line_end - line_start);
        if (line.size() >= kPrefix.size() && line.compare(0, kPrefix.size(), kPrefix) == 0) {
            size_t value_start = kPrefix.size();
            while (value_start < line.size() && line[value_start] == ' ') {
                ++value_start;
            }
            size_t value = 0;
            for (size_t index = value_start; index < line.size(); ++index) {
                if (line[index] < '0' || line[index] > '9') {
                    return std::nullopt;
                }
                value = value * 10 + static_cast<size_t>(line[index] - '0');
            }
            return value;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 2;
    }
    return std::nullopt;
}

std::string LanguageIdForBuffer(const Buffer& buffer) {
    switch (buffer.languageId()) {
        case LanguageId::C:
            return "c";
        case LanguageId::CHeader:
        case LanguageId::Cpp:
            return "cpp";
        default:
            return "plaintext";
    }
}

std::filesystem::path AbsolutePathForBuffer(const Buffer& buffer) {
    if (buffer.path().has_value()) {
        return std::filesystem::absolute(*buffer.path()).lexically_normal();
    }
    return std::filesystem::current_path() / buffer.name();
}

}  // namespace

ClangdClient::~ClangdClient() { Shutdown(); }

bool ClangdClient::Start(const std::filesystem::path& project_root, std::string* error) {
    if (IsStarted()) {
        return true;
    }
    ::signal(SIGPIPE, SIG_IGN);

    const std::optional<std::filesystem::path> executable = ResolveClangdExecutable();
    if (!executable.has_value()) {
        if (error != nullptr) {
            *error = "clangd not found. Install clangd or set PATCHWORK_CLANGD_PATH.";
        }
        return false;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) == -1 || ::pipe(stdout_pipe) == -1) {
        if (error != nullptr) {
            *error = std::string("Unable to create clangd pipes: ") + std::strerror(errno);
        }
        return false;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        if (error != nullptr) {
            *error = std::string("Unable to start clangd: ") + std::strerror(errno);
        }
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return false;
    }

    if (child == 0) {
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        const int dev_null = ::open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDERR_FILENO);
        }
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        if (!project_root.empty()) {
            ::chdir(project_root.c_str());
        }
        ::execl(executable->c_str(), executable->c_str(), "--log=error", nullptr);
        _exit(127);
    }

    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    input_fd_ = stdin_pipe[1];
    output_fd_ = stdout_pipe[0];
    child_pid_ = static_cast<int>(child);
    const int flags = ::fcntl(output_fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(output_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    JsonValue::Object capabilities;
    capabilities["textDocument"] = JsonValue(JsonValue::Object{});

    JsonValue::Object params;
    params["processId"] = JsonValue(static_cast<int>(::getpid()));
    params["rootUri"] = JsonValue(FileUriFromPath(project_root));
    params["capabilities"] = JsonValue(std::move(capabilities));

    std::string send_error;
    if (!SendRequest(next_request_id_++, "initialize", JsonValue(std::move(params)), &send_error) ||
        !SendNotification("initialized", JsonValue(JsonValue::Object{}), &send_error)) {
        Shutdown();
        if (error != nullptr) {
            *error = send_error.empty() ? "Unable to initialize clangd." : send_error;
        }
        return false;
    }
    return true;
}

bool ClangdClient::IsStarted() const { return input_fd_ >= 0 && output_fd_ >= 0 && child_pid_ > 0; }

bool ClangdClient::SyncDocument(const Buffer& buffer, std::string* error) {
    if (!IsStarted()) {
        if (error != nullptr) {
            *error = "clangd is not running.";
        }
        return false;
    }

    const std::string uri = FileUriFromPath(AbsolutePathForBuffer(buffer));
    if (current_uri_ != uri) {
        SendDidClose();
        current_uri_ = uri;
        document_version_ = 1;
        return SendDidOpen(buffer, uri, error);
    }
    ++document_version_;
    return SendDidChange(buffer, uri, error);
}

std::optional<int> ClangdClient::RequestCompletion(const Buffer& buffer, Cursor cursor, std::string* error) {
    if (!IsStarted()) {
        if (error != nullptr) {
            *error = "clangd is not running.";
        }
        return std::nullopt;
    }

    const std::string uri = FileUriFromPath(AbsolutePathForBuffer(buffer));
    JsonValue::Object context;
    context["triggerKind"] = JsonValue(1);

    JsonValue::Object params;
    params["textDocument"] = JsonValue(TextDocumentIdentifier(uri));
    params["position"] = JsonValue(PositionObject(cursor));
    params["context"] = JsonValue(std::move(context));

    const int request_id = next_request_id_++;
    if (!SendRequest(request_id, "textDocument/completion", JsonValue(std::move(params)), error)) {
        return std::nullopt;
    }
    return request_id;
}

std::vector<CompletionEvent> ClangdClient::PollEvents() {
    CheckProcessExit();
    ReadAvailableMessages();
    std::vector<CompletionEvent> events;
    events.reserve(queued_events_.size());
    while (!queued_events_.empty()) {
        events.push_back(std::move(queued_events_.front()));
        queued_events_.pop_front();
    }
    return events;
}

void ClangdClient::Shutdown() {
    if (IsStarted()) {
        std::string ignored;
        SendNotification("exit", JsonValue(JsonValue::Object{}), &ignored);
    }
    if (input_fd_ >= 0) {
        ::close(input_fd_);
        input_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        ::close(output_fd_);
        output_fd_ = -1;
    }
    if (child_pid_ > 0) {
        ::kill(child_pid_, SIGTERM);
        int status = 0;
        if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
            ::kill(child_pid_, SIGKILL);
            ::waitpid(child_pid_, nullptr, 0);
        }
        child_pid_ = -1;
    }
    current_uri_.clear();
    read_buffer_.clear();
    queued_events_.clear();
}

bool ClangdClient::SendMessage(const std::string& json, std::string* error) {
    const std::string framed = HeaderForBody(json) + json;
    if (!WriteAll(input_fd_, framed)) {
        if (error != nullptr) {
            *error = std::string("Unable to write to clangd: ") + std::strerror(errno);
        }
        return false;
    }
    return true;
}

bool ClangdClient::SendRequest(int id, const std::string& method, JsonValue params, std::string* error) {
    JsonValue::Object message;
    message["jsonrpc"] = JsonValue("2.0");
    message["id"] = JsonValue(id);
    message["method"] = JsonValue(method);
    message["params"] = std::move(params);
    return SendMessage(JsonValue(std::move(message)).Serialize(), error);
}

bool ClangdClient::SendNotification(const std::string& method, JsonValue params, std::string* error) {
    JsonValue::Object message;
    message["jsonrpc"] = JsonValue("2.0");
    message["method"] = JsonValue(method);
    message["params"] = std::move(params);
    return SendMessage(JsonValue(std::move(message)).Serialize(), error);
}

void ClangdClient::ReadAvailableMessages() {
    if (output_fd_ < 0) {
        return;
    }

    char buffer[4096];
    while (true) {
        const ssize_t bytes = ::read(output_fd_, buffer, sizeof(buffer));
        if (bytes > 0) {
            read_buffer_.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }
        if (bytes == 0 || (bytes == -1 && errno != EINTR)) {
            break;
        }
    }

    while (true) {
        const size_t header_end = read_buffer_.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            break;
        }
        const std::optional<size_t> content_length =
            ParseContentLength(std::string_view(read_buffer_).substr(0, header_end));
        if (!content_length.has_value()) {
            read_buffer_.erase(0, header_end + 4);
            continue;
        }
        const size_t body_start = header_end + 4;
        if (read_buffer_.size() < body_start + *content_length) {
            break;
        }

        const std::string body = read_buffer_.substr(body_start, *content_length);
        read_buffer_.erase(0, body_start + *content_length);
        std::string parse_error;
        const std::optional<JsonValue> message = JsonValue::Parse(body, &parse_error);
        if (message.has_value()) {
            HandleMessage(*message);
        }
    }
}

void ClangdClient::HandleMessage(const JsonValue& message) {
    const JsonValue* id = message.find("id");
    if (id == nullptr || !id->isNumber()) {
        return;
    }

    const int request_id = id->intValue();
    if (const JsonValue* error = message.find("error"); error != nullptr) {
        queued_events_.push_back({.kind = CompletionEventKind::Error,
                                  .request_id = request_id,
                                  .error_message = "clangd completion request failed."});
        return;
    }

    const JsonValue* result = message.find("result");
    if (result != nullptr) {
        HandleCompletionResponse(request_id, *result);
    }
}

void ClangdClient::HandleCompletionResponse(int request_id, const JsonValue& result) {
    queued_events_.push_back({
        .kind = CompletionEventKind::Completed,
        .request_id = request_id,
        .items = ParseCompletionItems(result),
    });
}

void ClangdClient::CheckProcessExit() {
    if (child_pid_ <= 0) {
        return;
    }

    int status = 0;
    const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
    if (result == 0 || result == -1) {
        return;
    }

    if (input_fd_ >= 0) {
        ::close(input_fd_);
        input_fd_ = -1;
    }
    if (output_fd_ >= 0) {
        ::close(output_fd_);
        output_fd_ = -1;
    }
    child_pid_ = -1;
    current_uri_.clear();
    read_buffer_.clear();
    queued_events_.push_back({.kind = CompletionEventKind::Error,
                              .request_id = 0,
                              .error_message = "clangd exited before completing the request."});
}

bool ClangdClient::SendDidOpen(const Buffer& buffer, const std::string& uri, std::string* error) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    text_document["languageId"] = JsonValue(LanguageIdForBuffer(buffer));
    text_document["version"] = JsonValue(document_version_);
    text_document["text"] = JsonValue(buffer.text());

    JsonValue::Object params;
    params["textDocument"] = JsonValue(std::move(text_document));
    return SendNotification("textDocument/didOpen", JsonValue(std::move(params)), error);
}

bool ClangdClient::SendDidChange(const Buffer& buffer, const std::string& uri, std::string* error) {
    JsonValue::Object text_document;
    text_document["uri"] = JsonValue(uri);
    text_document["version"] = JsonValue(document_version_);

    JsonValue::Object change;
    change["text"] = JsonValue(buffer.text());

    JsonValue::Object params;
    params["textDocument"] = JsonValue(std::move(text_document));
    params["contentChanges"] = JsonValue(JsonValue::Array{JsonValue(std::move(change))});
    return SendNotification("textDocument/didChange", JsonValue(std::move(params)), error);
}

void ClangdClient::SendDidClose() {
    if (current_uri_.empty() || !IsStarted()) {
        return;
    }

    JsonValue::Object params;
    params["textDocument"] = JsonValue(TextDocumentIdentifier(current_uri_));
    std::string ignored;
    SendNotification("textDocument/didClose", JsonValue(std::move(params)), &ignored);
}

std::filesystem::path ResolveClangdProjectRoot(const Buffer& buffer) {
    std::filesystem::path path = AbsolutePathForBuffer(buffer);
    if (!std::filesystem::is_directory(path)) {
        path = path.parent_path();
    }

    std::filesystem::path current = path;
    while (!current.empty()) {
        if (std::filesystem::exists(current / "compile_commands.json") ||
            std::filesystem::exists(current / "build" / "compile_commands.json") ||
            std::filesystem::exists(current / ".git")) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return path.empty() ? std::filesystem::current_path() : path;
}

std::string FileUriFromPath(const std::filesystem::path& path) {
    const std::filesystem::path absolute = std::filesystem::absolute(path).lexically_normal();
    std::ostringstream uri;
    uri << "file://";
    const std::string text = absolute.generic_string();
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char ch : text) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                                ch == '.' || ch == '~' || ch == '/';
        if (unreserved) {
            uri << static_cast<char>(ch);
        } else {
            uri << '%' << kHex[ch >> 4] << kHex[ch & 0x0F];
        }
    }
    return uri.str();
}

std::vector<CompletionItem> ParseCompletionItemsForTest(const JsonValue& result) {
    return ParseCompletionItems(result);
}

}  // namespace patchwork
