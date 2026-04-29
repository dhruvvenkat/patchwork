#include "json.h"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace patchwork {

namespace {

void AppendCodePointUtf8(unsigned int code_point, std::string* output) {
    if (output == nullptr) {
        return;
    }

    if (code_point <= 0x7F) {
        output->push_back(static_cast<char>(code_point));
        return;
    }
    if (code_point <= 0x7FF) {
        output->push_back(static_cast<char>(0xC0 | ((code_point >> 6) & 0x1F)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        return;
    }
    if (code_point <= 0xFFFF) {
        output->push_back(static_cast<char>(0xE0 | ((code_point >> 12) & 0x0F)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        return;
    }

    output->push_back(static_cast<char>(0xF0 | ((code_point >> 18) & 0x07)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    output->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
}

std::optional<unsigned int> ParseHexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned int>(10 + ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<unsigned int>(10 + ch - 'A');
    }
    return std::nullopt;
}

std::string EscapeJsonString(const std::string& input) {
    std::string escaped;
    escaped.reserve(input.size() + 8);
    for (unsigned char ch : input) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    std::ostringstream hex;
                    hex << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch);
                    escaped += hex.str();
                } else {
                    escaped.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return escaped;
}

class JsonParser {
  public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    std::optional<JsonValue> Parse(std::string* error) {
        SkipWhitespace();
        std::optional<JsonValue> value = ParseValue(error);
        if (!value.has_value()) {
            return std::nullopt;
        }

        SkipWhitespace();
        if (!AtEnd()) {
            if (error != nullptr) {
                *error = "Unexpected trailing JSON content.";
            }
            return std::nullopt;
        }
        return value;
    }

  private:
    bool AtEnd() const { return position_ >= input_.size(); }

    char Peek() const { return AtEnd() ? '\0' : input_[position_]; }

    char Consume() {
        if (AtEnd()) {
            return '\0';
        }
        return input_[position_++];
    }

    void SkipWhitespace() {
        while (!AtEnd() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    std::optional<JsonValue> ParseValue(std::string* error) {
        if (AtEnd()) {
            if (error != nullptr) {
                *error = "Unexpected end of JSON input.";
            }
            return std::nullopt;
        }

        switch (Peek()) {
            case 'n':
                if (ConsumeLiteral("null")) {
                    return JsonValue();
                }
                break;
            case 't':
                if (ConsumeLiteral("true")) {
                    return JsonValue(true);
                }
                break;
            case 'f':
                if (ConsumeLiteral("false")) {
                    return JsonValue(false);
                }
                break;
            case '"':
                return ParseString(error);
            case '[':
                return ParseArray(error);
            case '{':
                return ParseObject(error);
            default:
                if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                    return ParseNumber(error);
                }
                break;
        }

        if (error != nullptr) {
            *error = "Invalid JSON value.";
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseString(std::string* error) {
        if (Consume() != '"') {
            if (error != nullptr) {
                *error = "Expected JSON string.";
            }
            return std::nullopt;
        }

        std::string output;
        while (!AtEnd()) {
            const char ch = Consume();
            if (ch == '"') {
                return JsonValue(output);
            }
            if (ch != '\\') {
                output.push_back(ch);
                continue;
            }
            if (AtEnd()) {
                if (error != nullptr) {
                    *error = "Incomplete JSON escape sequence.";
                }
                return std::nullopt;
            }

            const char escaped = Consume();
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    if (position_ + 4 > input_.size()) {
                        if (error != nullptr) {
                            *error = "Invalid JSON unicode escape.";
                        }
                        return std::nullopt;
                    }
                    unsigned int code_point = 0;
                    for (int index = 0; index < 4; ++index) {
                        const std::optional<unsigned int> digit = ParseHexDigit(input_[position_ + index]);
                        if (!digit.has_value()) {
                            if (error != nullptr) {
                                *error = "Invalid JSON unicode escape.";
                            }
                            return std::nullopt;
                        }
                        code_point = (code_point << 4U) | *digit;
                    }
                    position_ += 4;
                    AppendCodePointUtf8(code_point, &output);
                    break;
                }
                default:
                    if (error != nullptr) {
                        *error = "Invalid JSON escape sequence.";
                    }
                    return std::nullopt;
            }
        }

        if (error != nullptr) {
            *error = "Unterminated JSON string.";
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseNumber(std::string* error) {
        const size_t start = position_;
        if (Peek() == '-') {
            ++position_;
        }
        if (Peek() == '0') {
            ++position_;
        } else {
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }
        if (!AtEnd() && Peek() == '.') {
            ++position_;
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }
        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++position_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                ++position_;
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }

        try {
            const double value = std::stod(std::string(input_.substr(start, position_ - start)));
            return JsonValue(value);
        } catch (const std::exception&) {
            if (error != nullptr) {
                *error = "Invalid JSON number.";
            }
            return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseArray(std::string* error) {
        if (Consume() != '[') {
            if (error != nullptr) {
                *error = "Expected JSON array.";
            }
            return std::nullopt;
        }

        JsonValue::Array values;
        SkipWhitespace();
        if (Peek() == ']') {
            Consume();
            return JsonValue(values);
        }

        while (true) {
            SkipWhitespace();
            std::optional<JsonValue> value = ParseValue(error);
            if (!value.has_value()) {
                return std::nullopt;
            }
            values.push_back(*value);

            SkipWhitespace();
            const char separator = Consume();
            if (separator == ']') {
                return JsonValue(values);
            }
            if (separator != ',') {
                if (error != nullptr) {
                    *error = "Expected ',' or ']' in JSON array.";
                }
                return std::nullopt;
            }
        }
    }

    std::optional<JsonValue> ParseObject(std::string* error) {
        if (Consume() != '{') {
            if (error != nullptr) {
                *error = "Expected JSON object.";
            }
            return std::nullopt;
        }

        JsonValue::Object values;
        SkipWhitespace();
        if (Peek() == '}') {
            Consume();
            return JsonValue(values);
        }

        while (true) {
            SkipWhitespace();
            std::optional<JsonValue> key = ParseString(error);
            if (!key.has_value()) {
                return std::nullopt;
            }

            SkipWhitespace();
            if (Consume() != ':') {
                if (error != nullptr) {
                    *error = "Expected ':' in JSON object.";
                }
                return std::nullopt;
            }

            SkipWhitespace();
            std::optional<JsonValue> value = ParseValue(error);
            if (!value.has_value()) {
                return std::nullopt;
            }
            values.emplace(key->stringValue(), *value);

            SkipWhitespace();
            const char separator = Consume();
            if (separator == '}') {
                return JsonValue(values);
            }
            if (separator != ',') {
                if (error != nullptr) {
                    *error = "Expected ',' or '}' in JSON object.";
                }
                return std::nullopt;
            }
        }
    }

    std::string_view input_;
    size_t position_ = 0;
};

std::string SerializeNumber(double value) {
    if (std::isfinite(value) == 0) {
        return "null";
    }
    if (std::fabs(value - std::round(value)) < std::numeric_limits<double>::epsilon()) {
        return std::to_string(static_cast<long long>(std::llround(value)));
    }

    std::ostringstream output;
    output << std::setprecision(15) << value;
    return output.str();
}

}  // namespace

JsonValue::JsonValue() = default;

JsonValue::JsonValue(std::nullptr_t) : type_(Type::Null) {}

JsonValue::JsonValue(bool value) : type_(Type::Bool), bool_value_(value) {}

JsonValue::JsonValue(int value) : type_(Type::Number), number_value_(static_cast<double>(value)) {}

JsonValue::JsonValue(double value) : type_(Type::Number), number_value_(value) {}

JsonValue::JsonValue(std::string value) : type_(Type::String), string_value_(std::move(value)) {}

JsonValue::JsonValue(const char* value)
    : type_(Type::String), string_value_(value == nullptr ? "" : value) {}

JsonValue::JsonValue(Array value) : type_(Type::Array), array_value_(std::move(value)) {}

JsonValue::JsonValue(Object value) : type_(Type::Object), object_value_(std::move(value)) {}

JsonValue::Type JsonValue::type() const { return type_; }

bool JsonValue::isNull() const { return type_ == Type::Null; }

bool JsonValue::isBool() const { return type_ == Type::Bool; }

bool JsonValue::isNumber() const { return type_ == Type::Number; }

bool JsonValue::isString() const { return type_ == Type::String; }

bool JsonValue::isArray() const { return type_ == Type::Array; }

bool JsonValue::isObject() const { return type_ == Type::Object; }

bool JsonValue::boolValue() const { return bool_value_; }

double JsonValue::numberValue() const { return number_value_; }

int JsonValue::intValue() const { return static_cast<int>(number_value_); }

const std::string& JsonValue::stringValue() const { return string_value_; }

const JsonValue::Array& JsonValue::arrayValue() const { return array_value_; }

const JsonValue::Object& JsonValue::objectValue() const { return object_value_; }

const JsonValue* JsonValue::find(std::string_view key) const {
    if (!isObject()) {
        return nullptr;
    }
    const auto found = object_value_.find(std::string(key));
    if (found == object_value_.end()) {
        return nullptr;
    }
    return &found->second;
}

std::string JsonValue::Serialize() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_value_ ? "true" : "false";
        case Type::Number:
            return SerializeNumber(number_value_);
        case Type::String:
            return "\"" + EscapeJsonString(string_value_) + "\"";
        case Type::Array: {
            std::ostringstream output;
            output << '[';
            for (size_t index = 0; index < array_value_.size(); ++index) {
                if (index > 0) {
                    output << ',';
                }
                output << array_value_[index].Serialize();
            }
            output << ']';
            return output.str();
        }
        case Type::Object: {
            std::ostringstream output;
            output << '{';
            bool first = true;
            for (const auto& [key, value] : object_value_) {
                if (!first) {
                    output << ',';
                }
                first = false;
                output << "\"" << EscapeJsonString(key) << "\":" << value.Serialize();
            }
            output << '}';
            return output.str();
        }
    }
    return "null";
}

std::optional<JsonValue> JsonValue::Parse(std::string_view text, std::string* error) {
    JsonParser parser(text);
    return parser.Parse(error);
}

}  // namespace patchwork
