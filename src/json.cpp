#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace ag {

static const Json kNull;
static const std::string kEmpty;

const std::string& Json::as_string() const { return t_ == String ? s_ : kEmpty; }

const Json& Json::at(size_t i) const {
    if (t_ != Array || i >= a_.size()) return kNull;
    return a_[i];
}
void Json::push_back(const Json& v) {
    if (t_ != Array) { *this = array(); }
    a_.push_back(v);
}
void Json::push_back(Json&& v) {
    if (t_ != Array) { *this = array(); }
    a_.push_back(std::move(v));
}
void Json::erase_range(size_t from, size_t to) {
    if (t_ != Array || from >= to || from >= a_.size()) return;
    if (to > a_.size()) to = a_.size();
    a_.erase(a_.begin() + from, a_.begin() + to);
}
bool Json::has(const std::string& k) const {
    if (t_ != Object) return false;
    for (auto& m : o_) if (m.first == k) return true;
    return false;
}
const Json& Json::operator[](const std::string& k) const {
    if (t_ != Object) return kNull;
    for (auto& m : o_) if (m.first == k) return m.second;
    return kNull;
}
Json* Json::find(const std::string& k) {
    if (t_ != Object) return nullptr;
    for (auto& m : o_) if (m.first == k) return &m.second;
    return nullptr;
}
Json& Json::set(const std::string& k, const Json& v) {
    if (t_ != Object) { *this = object(); }
    for (auto& m : o_) if (m.first == k) { m.second = v; return *this; }
    o_.emplace_back(k, v);
    return *this;
}
Json& Json::set(const std::string& k, Json&& v) {
    if (t_ != Object) { *this = object(); }
    for (auto& m : o_) if (m.first == k) { m.second = std::move(v); return *this; }
    o_.emplace_back(k, std::move(v));
    return *this;
}
void Json::remove(const std::string& k) {
    if (t_ != Object) return;
    for (size_t i = 0; i < o_.size(); i++)
        if (o_[i].first == k) { o_.erase(o_.begin() + i); return; }
}
std::string Json::get_str(const std::string& k, const std::string& def) const {
    const Json& v = (*this)[k];
    return v.is_string() ? v.s_ : def;
}
long long Json::get_int(const std::string& k, long long def) const {
    const Json& v = (*this)[k];
    return v.is_number() ? (long long)v.n_ : def;
}
bool Json::get_bool(const std::string& k, bool def) const {
    const Json& v = (*this)[k];
    return v.is_bool() ? v.b_ : def;
}

// ---------------------------------------------------------------- dump

static void put_u(std::string& out, unsigned cp) {
    char buf[8];
    snprintf(buf, sizeof buf, "\\u%04x", cp & 0xFFFF);
    out += buf;
}

// Decode one UTF-8 sequence at s[i]; returns code point or -1 if invalid; advances i.
static long decode_utf8(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i++; return c; }
    int len; unsigned cp;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { i++; return -1; }
    if (i + len > s.size()) { i++; return -1; }
    for (int k = 1; k < len; k++) {
        unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { i++; return -1; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) { i++; return -1; }
    i += len;
    return (long)cp;
}

void json_escape(std::string& out, const std::string& s, bool ascii) {
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            i++;
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20 || c == 0x7F) put_u(out, c);
                else out += (char)c;
            }
            continue;
        }
        size_t start = i;
        long cp = decode_utf8(s, i);
        if (cp < 0) {  // invalid byte -> U+FFFD
            if (ascii) out += "\\ufffd"; else out += "\xEF\xBF\xBD";
            continue;
        }
        if (!ascii) { out.append(s, start, i - start); continue; }
        if (cp >= 0x10000) {
            unsigned v = (unsigned)cp - 0x10000;
            put_u(out, 0xD800 + (v >> 10));
            put_u(out, 0xDC00 + (v & 0x3FF));
        } else {
            put_u(out, (unsigned)cp);
        }
    }
    out += '"';
}

void Json::dump_to(std::string& out, bool ascii) const {
    switch (t_) {
    case Null: out += "null"; break;
    case Bool: out += b_ ? "true" : "false"; break;
    case Number: {
        char buf[40];
        if (!std::isfinite(n_)) { out += "null"; break; }
        if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) snprintf(buf, sizeof buf, "%lld", (long long)n_);
        else snprintf(buf, sizeof buf, "%.17g", n_);
        out += buf;
        break;
    }
    case String: json_escape(out, s_, ascii); break;
    case Array:
        out += '[';
        for (size_t i = 0; i < a_.size(); i++) {
            if (i) out += ',';
            a_[i].dump_to(out, ascii);
        }
        out += ']';
        break;
    case Object:
        out += '{';
        for (size_t i = 0; i < o_.size(); i++) {
            if (i) out += ',';
            json_escape(out, o_[i].first, ascii);
            out += ':';
            o_[i].second.dump_to(out, ascii);
        }
        out += '}';
        break;
    }
}

std::string Json::dump(bool ascii) const {
    std::string out;
    dump_to(out, ascii);
    return out;
}

// ---------------------------------------------------------------- parse

namespace {
struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int depth = 0;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    static void put_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad \\u escape");
        }
        return true;
    }
    bool str(std::string& o) {
        p++;  // opening quote
        while (true) {
            const char* s = p;
            while (p < end && *p != '"' && *p != '\\') p++;
            o.append(s, p - s);
            if (p >= end) return fail("unterminated string");
            if (*p == '"') { p++; return true; }
            p++;  // backslash
            if (p >= end) return fail("bad escape");
            char c = *p++;
            switch (c) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    const char* save = p;
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { p = save; cp = 0xFFFD; }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                put_utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
    }
    bool value(Json& out) {
        ws();
        if (p >= end) return fail("unexpected end");
        if (++depth > 256) return fail("nesting too deep");
        bool ok = true;
        char c = *p;
        if (c == '{') {
            p++;
            out = Json::object();
            ws();
            if (p < end && *p == '}') { p++; depth--; return true; }
            while (ok) {
                ws();
                if (p >= end || *p != '"') return fail("expected key");
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= end || *p != ':') return fail("expected ':'");
                p++;
                Json v;
                if (!value(v)) return false;
                out.members().emplace_back(std::move(k), std::move(v));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; break; }
                return fail("expected ',' or '}'");
            }
        } else if (c == '[') {
            p++;
            out = Json::array();
            ws();
            if (p < end && *p == ']') { p++; depth--; return true; }
            while (true) {
                Json v;
                if (!value(v)) return false;
                out.items().push_back(std::move(v));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; break; }
                return fail("expected ',' or ']'");
            }
        } else if (c == '"') {
            std::string s;
            if (!str(s)) return false;
            out = Json(std::move(s));
        } else if (c == 't' && end - p >= 4 && !memcmp(p, "true", 4)) { p += 4; out = Json(true); }
        else if (c == 'f' && end - p >= 5 && !memcmp(p, "false", 5)) { p += 5; out = Json(false); }
        else if (c == 'n' && end - p >= 4 && !memcmp(p, "null", 4)) { p += 4; out = Json(); }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p;
            if (*p == '-') p++;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
            std::string num(s, p - s);
            char* e = nullptr;
            double d = strtod(num.c_str(), &e);
            if (!e || *e) return fail("bad number");
            out = Json(d);
        } else return fail("unexpected character");
        depth--;
        return ok;
    }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    Parser ps;
    ps.p = text.data();
    ps.end = text.data() + text.size();
    Json v;
    if (!ps.value(v)) {
        if (err) *err = ps.err + " at offset " + std::to_string(ps.p - text.data());
        return false;
    }
    ps.ws();
    if (ps.p != ps.end) {
        if (err) *err = "trailing data at offset " + std::to_string(ps.p - text.data());
        return false;
    }
    out = std::move(v);
    return true;
}

}  // namespace ag
