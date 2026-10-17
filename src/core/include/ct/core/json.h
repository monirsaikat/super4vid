#pragma once
// ct/core/json.h
// JSON DOM with an RFC 8259 parser and a deterministic writer (settings, projects, presets, recording metadata).
// Objects keep insertion order, so Parse -> Dump -> Parse -> Dump is byte-identical. Integers that fit int64_t are
// stored exactly (9007199254740993 survives); everything else is a double. Number parsing/printing always uses the
// C locale. Thread model: a Json value is a plain value type; concurrent reads are fine, writes need one owner.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ct {

class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    // UTF-8 input, optional BOM, depth limit 256, rejects trailing commas and trailing garbage.
    // On failure `out` is Null and *error is "line 3 col 14: expected ','".
    static bool Parse(std::string_view text, Json& out, std::string* error = nullptr);
    // indent 0 = compact. Integers exact; doubles shortest round-trip form; NaN/Inf written as null.
    std::string Dump(int indent = 2) const;

    Type GetType() const { return m_type; }
    bool IsNull() const { return m_type == Type::Null; }
    bool IsBool() const { return m_type == Type::Bool; }
    bool IsNumber() const { return m_type == Type::Number; }
    bool IsString() const { return m_type == Type::String; }
    bool IsArray() const { return m_type == Type::Array; }
    bool IsObject() const { return m_type == Type::Object; }
    bool IsInteger() const { return m_type == Type::Number && m_is_int; }   // stored as an exact int64

    bool AsBool(bool def = false) const;
    double AsNumber(double def = 0) const;
    int64_t AsInt64(int64_t def = 0) const;   // doubles truncate toward zero, clamped to the int64 range
    int AsInt(int def = 0) const;             // clamped to the int range
    const std::string& AsString() const;      // empty string when not a string
    std::string AsString(std::string_view def) const;

    const Json& operator[](std::string_view key) const;   // static Null for missing keys / non-objects
    const Json& operator[](size_t index) const;           // static Null when out of range / not an array
    Json& operator[](std::string_view key);               // creates (turns Null into an Object)
    Json& operator[](size_t index);                       // existing element; index must be < Size()

    bool Has(std::string_view key) const;
    size_t Size() const;                                   // elements of an array / members of an object, else 0
    Json& Push(Json v);                                    // turns Null into an Array; returns the new element
    Json& Set(std::string_view key, Json v);               // turns Null into an Object; replaces or appends
    bool Erase(std::string_view key);
    const std::vector<std::pair<std::string, Json>>& Items() const;   // insertion order preserved
    const std::vector<Json>& Elements() const;

    static Json Object();
    static Json Array();
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : m_type(Type::Bool), m_bool(v) {}
    Json(double v) : m_type(Type::Number), m_num(v) {}
    Json(int64_t v) : m_type(Type::Number), m_is_int(true), m_int(v) {}
    Json(int v) : m_type(Type::Number), m_is_int(true), m_int(v) {}
    Json(std::string v) : m_type(Type::String), m_str(std::move(v)) {}
    Json(std::string_view v) : m_type(Type::String), m_str(v) {}
    Json(const char* v) : m_type(Type::String), m_str(v ? v : "") {}
    // Other integer types (uint32_t, size_t, long, ...). uint64 values above INT64_MAX become doubles.
    template <class I, std::enable_if_t<std::is_integral_v<I> && !std::is_same_v<I, bool> && !std::is_same_v<I, int> &&
                                            !std::is_same_v<I, int64_t> && !std::is_same_v<I, char>,
                                        int> = 0>
    Json(I v) : m_type(Type::Number) {
        if constexpr (std::is_unsigned_v<I> && sizeof(I) >= sizeof(int64_t)) {
            if (v > static_cast<I>((std::numeric_limits<int64_t>::max)())) {
                m_num = static_cast<double>(v);
                return;
            }
        }
        m_is_int = true;
        m_int = static_cast<int64_t>(v);
    }

    bool operator==(const Json& other) const;   // deep; numbers compare by value (1 == 1.0)
    bool operator!=(const Json& other) const { return !(*this == other); }

    // helpers used by config/project code: `def` when the key is missing or has the wrong type.
    // T: bool, any integer (clamped), float/double, std::string.
    template <class T>
    T Get(std::string_view key, T def) const {
        const Json& v = (*this)[key];
        if constexpr (std::is_same_v<T, bool>) {
            return v.IsBool() ? v.m_bool : def;
        } else if constexpr (std::is_integral_v<T>) {
            if (!v.IsNumber()) return def;
            const int64_t i = v.AsInt64();
            if constexpr (std::is_unsigned_v<T>) {
                if (i < 0) return 0;
                if constexpr (sizeof(T) < sizeof(int64_t))
                    if (i > static_cast<int64_t>((std::numeric_limits<T>::max)())) return (std::numeric_limits<T>::max)();
                return static_cast<T>(i);
            } else {
                if constexpr (sizeof(T) < sizeof(int64_t)) {
                    if (i < static_cast<int64_t>((std::numeric_limits<T>::min)())) return (std::numeric_limits<T>::min)();
                    if (i > static_cast<int64_t>((std::numeric_limits<T>::max)())) return (std::numeric_limits<T>::max)();
                }
                return static_cast<T>(i);
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            return v.IsNumber() ? static_cast<T>(v.AsNumber()) : def;
        } else if constexpr (std::is_same_v<T, std::string>) {
            return v.IsString() ? v.m_str : def;
        } else {
            static_assert(std::is_same_v<T, bool>, "Json::Get supports bool, integers, floating point and std::string");
            return def;
        }
    }

private:
    friend class JsonParser;
    void DumpTo(std::string& out, int indent, int level) const;

    Type m_type = Type::Null;
    bool m_is_int = false;
    bool m_bool = false;
    double m_num = 0;
    int64_t m_int = 0;
    std::string m_str;
    std::vector<Json> m_arr;
    std::vector<std::pair<std::string, Json>> m_obj;
};

} // namespace ct
