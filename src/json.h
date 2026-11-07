// Minimal JSON value: parse, build, dump. Object keys keep insertion order.
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace ag {

class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : t_(Null), b_(false), n_(0) {}
    Json(std::nullptr_t) : Json() {}
    Json(bool b) : t_(Bool), b_(b), n_(0) {}
    Json(int n) : t_(Number), b_(false), n_(n) {}
    Json(long n) : t_(Number), b_(false), n_((double)n) {}
    Json(long long n) : t_(Number), b_(false), n_((double)n) {}
    Json(unsigned n) : t_(Number), b_(false), n_(n) {}
    Json(unsigned long n) : t_(Number), b_(false), n_((double)n) {}
    Json(unsigned long long n) : t_(Number), b_(false), n_((double)n) {}
    Json(double n) : t_(Number), b_(false), n_(n) {}
    Json(const char* s) : t_(String), b_(false), n_(0), s_(s) {}
    Json(const std::string& s) : t_(String), b_(false), n_(0), s_(s) {}
    Json(std::string&& s) : t_(String), b_(false), n_(0), s_(std::move(s)) {}

    static Json array() { Json j; j.t_ = Array; return j; }
    static Json object() { Json j; j.t_ = Object; return j; }

    // Parse; on failure returns false and sets *err.
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

    // ascii=true escapes every non-ASCII char as \uXXXX (surrogate pairs).
    std::string dump(bool ascii = false) const;
    void dump_to(std::string& out, bool ascii) const;

    Type type() const { return t_; }
    bool is_null() const { return t_ == Null; }
    bool is_bool() const { return t_ == Bool; }
    bool is_number() const { return t_ == Number; }
    bool is_string() const { return t_ == String; }
    bool is_array() const { return t_ == Array; }
    bool is_object() const { return t_ == Object; }

    bool as_bool(bool def = false) const { return t_ == Bool ? b_ : def; }
    double as_number(double def = 0) const { return t_ == Number ? n_ : def; }
    long long as_int(long long def = 0) const { return t_ == Number ? (long long)n_ : def; }
    const std::string& as_string() const;  // empty string if not a string
    std::string str_or(const std::string& def) const { return t_ == String ? s_ : def; }

    // Array access
    size_t size() const { return t_ == Array ? a_.size() : (t_ == Object ? o_.size() : 0); }
    const Json& at(size_t i) const;
    Json& at(size_t i) { return a_[i]; }
    void push_back(const Json& v);
    void push_back(Json&& v);
    std::vector<Json>& items() { return a_; }
    const std::vector<Json>& items() const { return a_; }
    void erase_range(size_t from, size_t to);  // array [from, to)

    // Object access
    bool has(const std::string& k) const;
    const Json& operator[](const std::string& k) const;  // null if missing
    const Json& operator[](const char* k) const { return (*this)[std::string(k)]; }
    Json& set(const std::string& k, const Json& v);  // returns *this for chaining
    Json& set(const std::string& k, Json&& v);
    Json* find(const std::string& k);
    void remove(const std::string& k);
    std::vector<std::pair<std::string, Json>>& members() { return o_; }
    const std::vector<std::pair<std::string, Json>>& members() const { return o_; }

    // Convenience getters on objects
    std::string get_str(const std::string& k, const std::string& def = "") const;
    long long get_int(const std::string& k, long long def = 0) const;
    bool get_bool(const std::string& k, bool def = false) const;

private:
    Type t_;
    bool b_;
    double n_;
    std::string s_;
    std::vector<Json> a_;
    std::vector<std::pair<std::string, Json>> o_;
};

// Append a JSON-escaped string literal (with quotes) to out.
void json_escape(std::string& out, const std::string& s, bool ascii);

}  // namespace ag
