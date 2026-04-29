#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace patchwork {

class JsonValue {
  public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    JsonValue();
    JsonValue(std::nullptr_t);
    JsonValue(bool value);
    JsonValue(int value);
    JsonValue(double value);
    JsonValue(std::string value);
    JsonValue(const char* value);
    JsonValue(Array value);
    JsonValue(Object value);

    Type type() const;

    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool boolValue() const;
    double numberValue() const;
    int intValue() const;
    const std::string& stringValue() const;
    const Array& arrayValue() const;
    const Object& objectValue() const;

    const JsonValue* find(std::string_view key) const;

    std::string Serialize() const;

    static std::optional<JsonValue> Parse(std::string_view text, std::string* error = nullptr);

  private:
    Type type_ = Type::Null;
    bool bool_value_ = false;
    double number_value_ = 0.0;
    std::string string_value_;
    Array array_value_;
    Object object_value_;
};

}  // namespace patchwork
