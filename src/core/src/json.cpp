#include <ct/core/json.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale.h>

namespace ct {

namespace {

constexpr int kMaxDepth = 256;

// strtod/printf honour setlocale(); JSON numbers must not. Created once, freed at exit (keeps the CRT leak check clean).
struct CLocale {
    _locale_t loc = _create_locale(LC_NUMERIC, "C");
    ~CLocale() {
        if (loc) _free_locale(loc);
    }
};

_locale_t NumericLocale() {
    static CLocale c;
    return c.loc;
}

const Json& NullJson() {
    static const Json null_value;
    return null_value;
}

const std::string& EmptyString() {
    static const std::string empty;
    return empty;
}

const std::vector<Json>& EmptyArray() {
    static const std::vector<Json> empty;
    return empty;
}

const std::vector<std::pair<std::string, Json>>& EmptyItems() {
    static const std::vector<std::pair<std::string, Json>> empty;
    return empty;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void AppendEscaped(std::string& out, const std::string& s) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t run = 0;   // start of the current run of characters that need no escaping
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(s, run, i - run);
        run = i + 1;
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            out.append("\\u00");
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
            break;
        }
    }
    out.append(s, run, s.size() - run);
    out.push_back('"');
}

void AppendDouble(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out.append("null");
        return;
    }
    char buf[40];
    // Shortest of %.15g / %.16g / %.17g that reads back to the same bits (%.17g always does).
    for (int precision = 15; precision <= 17; ++precision) {
        const int n = _snprintf_l(buf, sizeof(buf), "%.*g", NumericLocale(), precision, v);
        if (n <= 0) continue;
        if (precision == 17 || _strtod_l(buf, nullptr, NumericLocale()) == v) {
            out.append(buf, static_cast<size_t>(n));
            return;
        }
    }
}

void Indent(std::string& out, int indent, int level) {
    out.push_back('\n');
    out.append(static_cast<size_t>(indent) * static_cast<size_t>(level), ' ');
}

} // namespace

// ---- Parser --------------------------------------------------------------------------------------------------

class JsonParser {
public:
    JsonParser(std::string_view text) : m_begin(text.data()), m_p(text.data()), m_end(text.data() + text.size()) {}

    bool ParseDocument(Json& out, std::string* error) {
        out = Json();
        if (m_end - m_p >= 3 && static_cast<unsigned char>(m_p[0]) == 0xEF && static_cast<unsigned char>(m_p[1]) == 0xBB &&
            static_cast<unsigned char>(m_p[2]) == 0xBF)
            m_p += 3;
        bool ok = ParseValue(out);
        if (ok) {
            SkipWs();
            if (m_p != m_end) ok = Fail("unexpected characters after the document");
        }
        if (!ok) {
            out = Json();
            if (error) *error = ErrorText();
        }
        return ok;
    }

private:
    bool Fail(const char* msg) {
        if (!m_error) {
            m_error = msg;
            m_error_pos = m_p;
        }
        return false;
    }

    std::string ErrorText() const {
        int line = 1, col = 1;
        for (const char* q = m_begin; q < m_error_pos && q < m_end; ++q) {
            if (*q == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "line %d col %d: %s", line, col, m_error ? m_error : "error");
        return std::string(buf);
    }

    void SkipWs() {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r')) ++m_p;
    }

    bool Literal(const char* word, size_t len) {
        if (static_cast<size_t>(m_end - m_p) < len || std::memcmp(m_p, word, len) != 0) return Fail("invalid literal");
        m_p += len;
        return true;
    }

    bool ParseValue(Json& out) {
        SkipWs();
        if (m_p == m_end) return Fail("unexpected end of input");
        switch (*m_p) {
        case '{': return ParseObject(out);
        case '[': return ParseArray(out);
        case '"':
            out.m_type = Json::Type::String;
            return ParseString(out.m_str);
        case 't':
            out = Json(true);
            return Literal("true", 4);
        case 'f':
            out = Json(false);
            return Literal("false", 5);
        case 'n':
            out = Json();
            return Literal("null", 4);
        default:
            if (*m_p == '-' || (*m_p >= '0' && *m_p <= '9')) return ParseNumber(out);
            return Fail("unexpected character");
        }
    }

    bool ParseObject(Json& out) {
        if (++m_depth > kMaxDepth) return Fail("nesting deeper than 256 levels");
        out = Json::Object();
        ++m_p;   // '{'
        SkipWs();
        if (m_p < m_end && *m_p == '}') {
            ++m_p;
            --m_depth;
            return true;
        }
        for (;;) {
            SkipWs();
            if (m_p == m_end) return Fail("unexpected end of input");
            if (*m_p != '"') return Fail("expected a string key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (m_p == m_end || *m_p != ':') return Fail("expected ':'");
            ++m_p;
            out.m_obj.emplace_back(std::move(key), Json());
            if (!ParseValue(out.m_obj.back().second)) return false;
            SkipWs();
            if (m_p == m_end) return Fail("expected ',' or '}'");
            if (*m_p == ',') {
                ++m_p;
                SkipWs();
                if (m_p < m_end && *m_p == '}') return Fail("trailing comma");
                continue;
            }
            if (*m_p == '}') {
                ++m_p;
                break;
            }
            return Fail("expected ',' or '}'");
        }
        --m_depth;
        return true;
    }

    bool ParseArray(Json& out) {
        if (++m_depth > kMaxDepth) return Fail("nesting deeper than 256 levels");
        out = Json::Array();
        ++m_p;   // '['
        SkipWs();
        if (m_p < m_end && *m_p == ']') {
            ++m_p;
            --m_depth;
            return true;
        }
        for (;;) {
            out.m_arr.emplace_back();
            if (!ParseValue(out.m_arr.back())) return false;
            SkipWs();
            if (m_p == m_end) return Fail("expected ',' or ']'");
            if (*m_p == ',') {
                ++m_p;
                SkipWs();
                if (m_p < m_end && *m_p == ']') return Fail("trailing comma");
                continue;
            }
            if (*m_p == ']') {
                ++m_p;
                break;
            }
            return Fail("expected ',' or ']'");
        }
        --m_depth;
        return true;
    }

    bool Hex4(uint32_t& v) {
        if (m_end - m_p < 4) return Fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("invalid \\u escape");
        }
        m_p += 4;
        return true;
    }

    bool ParseString(std::string& out) {
        ++m_p;   // opening quote
        out.clear();
        for (;;) {
            const char* run = m_p;
            while (m_p < m_end && *m_p != '"' && *m_p != '\\' && static_cast<unsigned char>(*m_p) >= 0x20) ++m_p;
            out.append(run, static_cast<size_t>(m_p - run));
            if (m_p == m_end) return Fail("unterminated string");
            const char c = *m_p;
            if (c == '"') {
                ++m_p;
                return true;
            }
            if (c != '\\') return Fail("control character in string");
            ++m_p;
            if (m_p == m_end) return Fail("unterminated string");
            const char e = *m_p++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate: combine with a following \uDC00-\uDFFF, else it is unpaired.
                    uint32_t lo = 0;
                    if (m_end - m_p >= 6 && m_p[0] == '\\' && m_p[1] == 'u') {
                        const char* save = m_p;
                        m_p += 2;
                        if (!Hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            m_p = save;   // not a low surrogate: re-read it as its own escape
                            cp = 0xFFFD;
                        }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;   // unpaired low surrogate
                }
                AppendUtf8(out, cp);
                break;
            }
            default: --m_p; return Fail("invalid escape");
            }
        }
    }

    bool ParseNumber(Json& out) {
        const char* start = m_p;
        bool negative = false;
        if (*m_p == '-') {
            negative = true;
            ++m_p;
        }
        if (m_p == m_end) return Fail("invalid number");
        if (*m_p == '0') {
            ++m_p;
        } else if (*m_p >= '1' && *m_p <= '9') {
            while (m_p < m_end && *m_p >= '0' && *m_p <= '9') ++m_p;
        } else {
            return Fail("invalid number");
        }
        const char* int_end = m_p;
        bool is_int = true;
        if (m_p < m_end && *m_p == '.') {
            is_int = false;
            ++m_p;
            if (m_p == m_end || *m_p < '0' || *m_p > '9') return Fail("expected a digit after '.'");
            while (m_p < m_end && *m_p >= '0' && *m_p <= '9') ++m_p;
        }
        if (m_p < m_end && (*m_p == 'e' || *m_p == 'E')) {
            is_int = false;
            ++m_p;
            if (m_p < m_end && (*m_p == '+' || *m_p == '-')) ++m_p;
            if (m_p == m_end || *m_p < '0' || *m_p > '9') return Fail("expected a digit in the exponent");
            while (m_p < m_end && *m_p >= '0' && *m_p <= '9') ++m_p;
        }
        if (is_int) {
            // Exact int64 when it fits; "-0" stays a double so its sign survives.
            const char* d = negative ? start + 1 : start;
            uint64_t v = 0;
            bool overflow = false;
            for (; d < int_end; ++d) {
                const uint64_t digit = static_cast<uint64_t>(*d - '0');
                if (v > (UINT64_MAX - digit) / 10) {
                    overflow = true;
                    break;
                }
                v = v * 10 + digit;
            }
            const uint64_t limit = negative ? (uint64_t(1) << 63) : static_cast<uint64_t>(INT64_MAX);
            if (!overflow && v <= limit && !(negative && v == 0)) {
                out = Json(negative ? static_cast<int64_t>(0ull - v) : static_cast<int64_t>(v));
                return true;
            }
        }
        // strtod needs a terminated buffer; the view may continue with more digits beyond this token.
        const size_t len = static_cast<size_t>(m_p - start);
        char stack[128];
        std::string heap;
        const char* text = stack;
        if (len < sizeof(stack)) {
            std::memcpy(stack, start, len);
            stack[len] = '\0';
        } else {
            heap.assign(start, len);
            text = heap.c_str();
        }
        out = Json(_strtod_l(text, nullptr, NumericLocale()));   // overflow gives +-HUGE_VAL (dumped as null)
        return true;
    }

    const char* m_begin;
    const char* m_p;
    const char* m_end;
    int m_depth = 0;
    const char* m_error = nullptr;
    const char* m_error_pos = nullptr;
};

bool Json::Parse(std::string_view text, Json& out, std::string* error) {
    JsonParser parser(text);
    return parser.ParseDocument(out, error);
}

// ---- Writer --------------------------------------------------------------------------------------------------

std::string Json::Dump(int indent) const {
    std::string out;
    DumpTo(out, indent < 0 ? 0 : indent, 0);
    return out;
}

void Json::DumpTo(std::string& out, int indent, int level) const {
    switch (m_type) {
    case Type::Null: out.append("null"); break;
    case Type::Bool: out.append(m_bool ? "true" : "false"); break;
    case Type::Number:
        if (m_is_int) {
            char buf[24];
            const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_int));
            out.append(buf, static_cast<size_t>(n));
        } else {
            AppendDouble(out, m_num);
        }
        break;
    case Type::String: AppendEscaped(out, m_str); break;
    case Type::Array:
        if (m_arr.empty()) {
            out.append("[]");
            break;
        }
        out.push_back('[');
        for (size_t i = 0; i < m_arr.size(); ++i) {
            if (i) out.push_back(',');
            if (indent) Indent(out, indent, level + 1);
            m_arr[i].DumpTo(out, indent, level + 1);
        }
        if (indent) Indent(out, indent, level);
        out.push_back(']');
        break;
    case Type::Object:
        if (m_obj.empty()) {
            out.append("{}");
            break;
        }
        out.push_back('{');
        for (size_t i = 0; i < m_obj.size(); ++i) {
            if (i) out.push_back(',');
            if (indent) Indent(out, indent, level + 1);
            AppendEscaped(out, m_obj[i].first);
            out.append(indent ? ": " : ":");
            m_obj[i].second.DumpTo(out, indent, level + 1);
        }
        if (indent) Indent(out, indent, level);
        out.push_back('}');
        break;
    }
}

// ---- Accessors -----------------------------------------------------------------------------------------------

bool Json::AsBool(bool def) const { return m_type == Type::Bool ? m_bool : def; }

double Json::AsNumber(double def) const {
    if (m_type != Type::Number) return def;
    return m_is_int ? static_cast<double>(m_int) : m_num;
}

int64_t Json::AsInt64(int64_t def) const {
    if (m_type != Type::Number) return def;
    if (m_is_int) return m_int;
    if (std::isnan(m_num)) return def;
    if (m_num >= 9223372036854775807.0) return INT64_MAX;
    if (m_num <= -9223372036854775808.0) return INT64_MIN;
    return static_cast<int64_t>(m_num);
}

int Json::AsInt(int def) const {
    if (m_type != Type::Number) return def;
    const int64_t v = AsInt64(def);
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return static_cast<int>(v);
}

const std::string& Json::AsString() const { return m_type == Type::String ? m_str : EmptyString(); }

std::string Json::AsString(std::string_view def) const { return m_type == Type::String ? m_str : std::string(def); }

const Json& Json::operator[](std::string_view key) const {
    if (m_type == Type::Object)
        for (const auto& kv : m_obj)
            if (kv.first == key) return kv.second;
    return NullJson();
}

const Json& Json::operator[](size_t index) const {
    if (m_type == Type::Array && index < m_arr.size()) return m_arr[index];
    return NullJson();
}

Json& Json::operator[](std::string_view key) {
    if (m_type != Type::Object) *this = Object();
    for (auto& kv : m_obj)
        if (kv.first == key) return kv.second;
    m_obj.emplace_back(std::string(key), Json());
    return m_obj.back().second;
}

Json& Json::operator[](size_t index) { return m_arr[index]; }

bool Json::Has(std::string_view key) const {
    if (m_type != Type::Object) return false;
    for (const auto& kv : m_obj)
        if (kv.first == key) return true;
    return false;
}

size_t Json::Size() const {
    if (m_type == Type::Array) return m_arr.size();
    if (m_type == Type::Object) return m_obj.size();
    return 0;
}

Json& Json::Push(Json v) {
    if (m_type != Type::Array) *this = Array();
    m_arr.push_back(std::move(v));
    return m_arr.back();
}

Json& Json::Set(std::string_view key, Json v) {
    Json& slot = (*this)[key];
    slot = std::move(v);
    return slot;
}

bool Json::Erase(std::string_view key) {
    if (m_type != Type::Object) return false;
    for (auto it = m_obj.begin(); it != m_obj.end(); ++it) {
        if (it->first == key) {
            m_obj.erase(it);
            return true;
        }
    }
    return false;
}

const std::vector<std::pair<std::string, Json>>& Json::Items() const {
    return m_type == Type::Object ? m_obj : EmptyItems();
}

const std::vector<Json>& Json::Elements() const { return m_type == Type::Array ? m_arr : EmptyArray(); }

Json Json::Object() {
    Json j;
    j.m_type = Type::Object;
    return j;
}

Json Json::Array() {
    Json j;
    j.m_type = Type::Array;
    return j;
}

bool Json::operator==(const Json& o) const {
    if (m_type != o.m_type) return false;
    switch (m_type) {
    case Type::Null: return true;
    case Type::Bool: return m_bool == o.m_bool;
    case Type::Number:
        if (m_is_int && o.m_is_int) return m_int == o.m_int;
        return AsNumber() == o.AsNumber();
    case Type::String: return m_str == o.m_str;
    case Type::Array: return m_arr == o.m_arr;
    case Type::Object: return m_obj == o.m_obj;
    }
    return false;
}

} // namespace ct
