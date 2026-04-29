#include "buffer.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace patchwork {

namespace {

std::vector<std::string> EnsureNonEmpty(std::vector<std::string> lines) {
    if (lines.empty()) {
        lines.emplace_back();
    }
    return lines;
}

std::string ReadFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "Unable to open file: " + path.string();
        }
        return {};
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

}  // namespace

Buffer::Buffer(BufferType type, std::string name, bool read_only)
    : type_(type), name_(std::move(name)), read_only_(read_only), lines_(1, "") {}

BufferType Buffer::type() const { return type_; }

bool Buffer::readOnly() const { return read_only_; }

void Buffer::setReadOnly(bool read_only) { read_only_ = read_only; }

const std::string& Buffer::name() const { return name_; }

void Buffer::setName(std::string name) { name_ = std::move(name); }

const std::optional<std::filesystem::path>& Buffer::path() const { return path_; }

void Buffer::setPath(const std::filesystem::path& path) {
    path_ = path;
    if (name_.empty()) {
        name_ = path.filename().string();
    }
}

bool Buffer::dirty() const { return dirty_; }

void Buffer::clearDirty() { dirty_ = false; }

size_t Buffer::lineCount() const { return lines_.size(); }

const std::string& Buffer::line(size_t row) const {
    static const std::string kEmpty;
    if (row >= lines_.size()) {
        return kEmpty;
    }
    return lines_[row];
}

std::string Buffer::currentLineText(size_t row) const { return line(row); }

const std::vector<std::string>& Buffer::lines() const { return lines_; }

void Buffer::setLines(std::vector<std::string> lines, bool mark_dirty) {
    lines_ = EnsureNonEmpty(std::move(lines));
    if (mark_dirty && !read_only_) {
        dirty_ = true;
    }
}

void Buffer::setText(const std::string& text, bool mark_dirty) {
    setLines(SplitLines(text), mark_dirty);
}

std::string Buffer::text() const {
    std::ostringstream output;
    for (size_t index = 0; index < lines_.size(); ++index) {
        if (index > 0) {
            output << '\n';
        }
        output << lines_[index];
    }
    return output.str();
}

void Buffer::insertChar(const Cursor& position, char ch) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    const size_t row = std::min(position.row, lines_.size() - 1);
    const size_t col = std::min(position.col, lines_[row].size());
    lines_[row].insert(lines_[row].begin() + static_cast<std::ptrdiff_t>(col), ch);
    dirty_ = true;
}

void Buffer::insertNewline(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();

    const size_t row = std::min(cursor.row, lines_.size() - 1);
    const size_t col = std::min(cursor.col, lines_[row].size());
    std::string remainder = lines_[row].substr(col);
    lines_[row].erase(col);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(row + 1), remainder);
    cursor.row = row + 1;
    cursor.col = 0;
    dirty_ = true;
}

void Buffer::deleteCharBefore(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();
    if (cursor.row >= lines_.size()) {
        cursor.row = lines_.size() - 1;
        cursor.col = lines_[cursor.row].size();
        return;
    }

    if (cursor.col > 0) {
        lines_[cursor.row].erase(cursor.col - 1, 1);
        --cursor.col;
        dirty_ = true;
        return;
    }

    if (cursor.row == 0) {
        return;
    }

    const size_t previous_row = cursor.row - 1;
    const size_t previous_size = lines_[previous_row].size();
    lines_[previous_row] += lines_[cursor.row];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor.row));
    cursor.row = previous_row;
    cursor.col = previous_size;
    ensureNonEmpty();
    dirty_ = true;
}

void Buffer::deleteCharAt(Cursor& cursor) {
    if (read_only_) {
        return;
    }
    ensureNonEmpty();
    if (cursor.row >= lines_.size()) {
        return;
    }

    if (cursor.col < lines_[cursor.row].size()) {
        lines_[cursor.row].erase(cursor.col, 1);
        dirty_ = true;
        return;
    }

    if (cursor.row + 1 >= lines_.size()) {
        return;
    }

    lines_[cursor.row] += lines_[cursor.row + 1];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor.row + 1));
    ensureNonEmpty();
    dirty_ = true;
}

bool Buffer::save(std::string* error) {
    if (read_only_) {
        if (error != nullptr) {
            *error = "Buffer is read-only.";
        }
        return false;
    }
    if (!path_.has_value()) {
        if (error != nullptr) {
            *error = "No file path set.";
        }
        return false;
    }

    std::ofstream output(*path_);
    if (!output) {
        if (error != nullptr) {
            *error = "Unable to write file: " + path_->string();
        }
        return false;
    }

    output << text();
    output.flush();
    dirty_ = false;
    return output.good();
}

std::string Buffer::guessLanguage() const {
    static const std::unordered_map<std::string, std::string> kExtensions = {
        {".c", "C"},        {".cc", "C++"},      {".cpp", "C++"},   {".cxx", "C++"},
        {".h", "C/C++"},    {".hpp", "C++"},     {".rs", "Rust"},   {".py", "Python"},
        {".js", "JavaScript"}, {".ts", "TypeScript"}, {".java", "Java"},
        {".go", "Go"},      {".md", "Markdown"}, {".txt", "Text"},
    };

    if (!path_.has_value()) {
        return "Text";
    }

    const std::string extension = path_->extension().string();
    auto found = kExtensions.find(extension);
    if (found != kExtensions.end()) {
        return found->second;
    }
    return "Text";
}

void Buffer::ensureNonEmpty() {
    if (lines_.empty()) {
        lines_.emplace_back();
    }
}

Buffer LoadFileBuffer(const std::filesystem::path& path, std::string* error) {
    Buffer buffer(BufferType::File, path.filename().string(), false);
    buffer.setPath(path);

    if (!std::filesystem::exists(path)) {
        buffer.setLines({""}, false);
        buffer.clearDirty();
        return buffer;
    }

    std::string read_error;
    const std::string contents = ReadFile(path, &read_error);
    if (!read_error.empty()) {
        if (error != nullptr) {
            *error = read_error;
        }
        return buffer;
    }

    buffer.setText(contents, false);
    buffer.clearDirty();
    return buffer;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;

    for (char ch : text) {
        if (ch == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch != '\r') {
            current.push_back(ch);
        }
    }

    lines.push_back(current);
    return EnsureNonEmpty(std::move(lines));
}

}  // namespace patchwork

