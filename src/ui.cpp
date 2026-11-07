// Interactive console frontend. See NOTES.md ("Frontend") for the layout rules.
//
// Screen model: the transcript grows downwards from row 0 (append-only). Below it, after one blank
// row, sits the "live area": the input editor when idle, the thinking ticker or "allow? y/n" while
// busy, or the /resume picker. The only rows ever rewritten in place are the open (streaming) text
// row, the running tool's row (its timer), the "! can't reach endpoint" row, and the live area.
//
// Threads: none are created here. Ctrl+C arrives as a key event (processed input is off), so the
// system never injects a handler thread for it; the console control handler only takes the
// window-close / logoff / shutdown notifications.
#include "ui.h"
#include "agent.h"
#include "console.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace ag {

namespace {

volatile LONG g_close = 0;
HANDLE g_done = nullptr;

BOOL WINAPI ui_ctrl_handler(DWORD type) {
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        // Runs on a system-created thread. The main loop notices the flag within 50 ms, ends the
        // conversation through the backend and signals g_done; the process may exit after that.
        InterlockedExchange(&g_close, 1);
        if (g_done) WaitForSingleObject(g_done, 4500);
        return TRUE;
    }
    return TRUE;  // Ctrl+Break: ignore (Ctrl+C is read as a key, not a signal)
}

enum Kind { K_NONE, K_USER, K_TOOL, K_TEXT, K_ERR, K_SYS };

struct Mark {
    long long row = 0;
    Kind last = K_NONE;
    bool valid = false;
};

const wchar_t* kHelp = L"/new  /resume  /undo  /clear  /yolo";

std::wstring wnum(long long n) { return widen(std::to_string(n)); }

std::wstring clip(const std::wstring& s, int w) {
    if (w <= 0) return std::wstring();
    if ((int)s.size() <= w) return s;
    if (w <= 3) return s.substr(0, (size_t)w);
    return s.substr(0, (size_t)w - 3) + L"...";
}

// Collapse whitespace runs to one space and trim.
std::wstring flat(const std::wstring& s) {
    std::wstring o;
    bool sp = false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\n' || c == L'\t' || c == L'\r') { sp = !o.empty(); continue; }
        if (sp) o += L' ';
        sp = false;
        o += c;
    }
    return o;
}

std::wstring expand_tabs(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\t') o += L"    ";
        else o += c;
    }
    return o;
}

std::wstring dur(unsigned long ms) {
    unsigned long t = ms / 1000;
    if (t < 60) return wnum(t) + L"s";
    unsigned long s = t % 60;
    return wnum(t / 60) + L"m" + (s < 10 ? L"0" : L"") + wnum(s) + L"s";
}

// Word wrap (used for request bars): keeps a paragraph's leading indent on its continuation lines.
std::vector<std::wstring> wrap(const std::wstring& text, int w) {
    std::vector<std::wstring> out;
    if (w < 4) w = 4;
    size_t p = 0;
    while (true) {
        size_t nl = text.find(L'\n', p);
        std::wstring para = text.substr(p, nl == std::wstring::npos ? std::wstring::npos : nl - p);
        if ((int)para.size() <= w) {
            out.push_back(para);
        } else {
            size_t ind = para.find_first_not_of(L' ');
            if (ind == std::wstring::npos) ind = 0;
            if ((int)ind > w / 2) ind = (size_t)w / 2;
            std::wstring indent(ind, L' ');
            std::wstring line = indent;
            bool has = false;
            size_t i = 0;
            while (i < para.size()) {
                while (i < para.size() && para[i] == L' ') i++;
                size_t j = i;
                while (j < para.size() && para[j] != L' ') j++;
                if (j == i) break;
                std::wstring word = para.substr(i, j - i);
                i = j;
                int room = w - (int)ind;
                while ((int)word.size() > room) {
                    if (has) { out.push_back(line); line = indent; has = false; }
                    out.push_back(indent + word.substr(0, (size_t)room));
                    word = word.substr((size_t)room);
                }
                if (word.empty()) continue;
                std::wstring next = has ? line + L" " + word : line + word;
                if ((int)next.size() <= w) { line = next; has = true; }
                else { out.push_back(line); line = indent + word; has = true; }
            }
            out.push_back(line);
        }
        if (nl == std::wstring::npos) break;
        p = nl + 1;
    }
    return out;
}

// "2026-11-07T17:22:01Z" -> seconds since 1601 (UTC); 0 if unparsable.
long long iso_secs(const std::string& s) {
    SYSTEMTIME st = {};
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)se;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL);
}

// "2026-11-07T17:35:43.406Z" -> milliseconds (only differences are used).
long long iso_ms(const std::string& s) {
    long long sec = iso_secs(s);
    size_t dot = s.find('.');
    long long ms = dot != std::string::npos ? atoll(s.substr(dot + 1, 3).c_str()) : 0;
    return sec * 1000 + ms;
}

long long now_secs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL);
}

std::wstring age(long long secs) {
    if (secs < 0) secs = 0;
    long long m = (secs + 30) / 60;
    if (m < 60) return wnum(m) + L"m";
    long long h = (m + 30) / 60;
    if (h < 48) return wnum(h) + L"h";
    return wnum((h + 12) / 24) + L"d";
}

// The one argument worth showing on a tool line.
std::string short_args(const std::string& name, const Json& args) {
    if (args.is_string()) return args.as_string();
    if (!args.is_object()) return args.dump();
    const char* key = name == "bash" ? "command" : name == "web_search" ? "query" : name == "web_scrape" ? "url"
                    : (name == "read_file" || name == "write_file" || name == "edit_file") ? "path" : nullptr;
    if (key && args[key].is_string()) return args[key].as_string();
    Json c = args;
    c.remove("_step");
    return c.dump();
}

// One-line form of a backend error. Connection problems all read the same.
std::string short_error(std::string m) {
    if (starts_with(m, "LLM request failed: ")) m = m.substr(20);
    if (starts_with(m, "connection failed") || starts_with(m, "request failed")) return "can't reach endpoint";
    size_t nl = m.find('\n');
    if (nl != std::string::npos) m = m.substr(0, nl);
    return m;
}

bool unreachable_error(const std::string& e) {
    return starts_with(e, "connection failed") || starts_with(e, "request failed");
}

// First user message of a turn that wasn't undone; false if the session has none (empty).
// A cheap line scan: only the matching user line is parsed.
bool live_title(const std::string& path, std::string& title) {
    std::string data;
    if (file_size(path) > 8 * 1024 * 1024 || !read_file(path, data)) return !title.empty();
    std::set<long long> undone;
    std::vector<std::pair<long long, size_t>> users;  // turn, line offset
    size_t p = 0;
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        static const std::string kUndo = "{\"t\":\"undo\",\"turn\":", kMsg = "{\"t\":\"msg\",\"turn\":",
                                 kUser = ",\"msg\":{\"role\":\"user\"";
        if (data.compare(p, kUndo.size(), kUndo) == 0) {
            undone.insert(atoll(data.c_str() + p + kUndo.size()));
        } else if (data.compare(p, kMsg.size(), kMsg) == 0) {
            size_t q = data.find(',', p + kMsg.size());
            if (q != std::string::npos && q < e && data.compare(q, kUser.size(), kUser) == 0)
                users.push_back(std::make_pair(atoll(data.c_str() + p + kMsg.size()), p));
        }
        p = e + 1;
    }
    for (auto& u : users) {
        if (undone.count(u.first)) continue;
        size_t e = data.find('\n', u.second);
        Json j;
        if (!Json::parse(data.substr(u.second, e == std::string::npos ? std::string::npos : e - u.second), j)) continue;
        title = j["msg"].get_str("content");
        return true;
    }
    return false;
}

// Split a UTF-8 string so that the head doesn't end inside a sequence.
size_t utf8_cut(const std::string& s, size_t n) {
    if (n >= s.size()) return s.size();
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

class UI {
public:
    explicit UI(Agent& a) : ag_(a) {}
    int run();

private:
    Agent& ag_;
    Screen scr_;
    HANDLE in_ = INVALID_HANDLE_VALUE;
    bool quit_ = false;

    // ---- transcript
    long long trow_ = 0;          // the open row, or the next row to write
    bool open_ = false;
    std::wstring line_;
    bool dirty_ = false;
    Kind last_ = K_NONE;
    long long next_free() const { return open_ ? trow_ + 1 : trow_; }

    // streaming text writer
    bool in_text_ = false;
    int pend_nl_ = 0;
    int lead_ = 0;                // leading spaces before a block's first visible char
    bool para_start_ = true;
    int para_spaces_ = 0;
    int indent_ = 0;              // indent for wrapped continuation rows of this paragraph
    int row_indent_ = 0;          // indent of the open row
    bool in_word_ = false;
    size_t word_start_ = 0;
    Mark text_mark_;              // where the current attempt's text began (for discard_partial)
    int tag_state_ = 0;           // <think> filter: 0 undecided, 1 inside, 2 plain
    std::string tag_buf_;

    // ---- live area: virtual row -> (cells, inverted) as drawn
    std::map<long long, std::pair<std::wstring, bool>> live_;

    // ---- turn state
    bool busy_ = false;
    bool cancel_sent_ = false;
    bool err_printed_ = false;
    std::string approval_;
    bool thinking_ = false;
    std::wstring think_;
    std::set<std::string> skip_ids_;
    struct Tool {
        bool active = false;
        bool started = false;     // false while waiting for approval
        std::string id, name;
        long long row = 0;
        std::wstring left, shown;
        unsigned long t0 = 0;
    } tool_;
    bool unreach_ = false;
    Mark unreach_mark_;
    std::wstring ctx_ = L"ctx 0%";

    // ---- editor
    std::wstring ed_;
    size_t caret_ = 0;

    // ---- /resume picker
    bool picker_ = false;
    std::vector<SessionInfo> items_;
    int sel_ = 0, top_ = 0;

    int W() const { return scr_.width(); }

    // transcript primitives
    void claim(long long r, bool blank) {
        auto it = live_.find(r);
        if (it != live_.end()) {
            if (blank) scr_.row(r, std::wstring(), false);
            live_.erase(it);
        }
    }
    void t_row(const std::wstring& s, bool inv) {
        claim(trow_, false);
        scr_.row(trow_, s, inv);
        trow_++;
    }
    void t_blank() {
        claim(trow_, true);
        trow_++;
    }
    void flush() {
        if (open_ && dirty_) {
            claim(trow_, false);
            scr_.row(trow_, line_, false);
            dirty_ = false;
        }
    }
    void open_line() {
        open_ = true;
        line_.clear();
        dirty_ = true;
        claim(trow_, false);
    }
    void close_line() {
        if (!open_) return;
        flush();
        open_ = false;
        trow_++;
    }
    void begin(Kind k) {
        text_end();
        close_line();
        if (last_ != K_NONE && (k != last_ || k == K_USER)) t_blank();
        last_ = k;
    }
    Mark mark() {
        close_line();
        Mark m;
        m.row = trow_;
        m.last = last_;
        m.valid = true;
        return m;
    }
    // Remove everything printed since m.
    void rewind(const Mark& m) {
        if (!m.valid) return;
        in_text_ = false;
        pend_nl_ = 0;
        lead_ = 0;
        close_line();
        for (long long r = m.row; r < trow_; r++) {
            scr_.row(r, std::wstring(), false);
            live_.erase(r);
        }
        trow_ = m.row;
        last_ = m.last;
    }

    void reset_text_state() {
        in_text_ = false;
        pend_nl_ = 0;
        lead_ = 0;
        in_word_ = false;
        text_mark_.valid = false;
        tag_state_ = 0;
        tag_buf_.clear();
    }
    void text_end() {
        if (!in_text_) { lead_ = 0; pend_nl_ = 0; return; }
        in_text_ = false;
        pend_nl_ = 0;
        lead_ = 0;
        in_word_ = false;
        close_line();
    }
    void new_row_indented() {
        close_line();
        open_line();
        line_.assign((size_t)indent_, L' ');
        row_indent_ = indent_;
    }
    void text_char(wchar_t c);
    void text_cells(const std::wstring& cells) {
        for (wchar_t c : cells) text_char(c);
    }
    void on_text(const std::string& utf8);
    void add_reasoning(const std::string& utf8);

    void print_user(const std::string& utf8) {
        begin(K_USER);
        std::vector<std::wstring> ls = wrap(expand_tabs(to_cells(utf8)), W() - 2);
        for (size_t i = 0; i < ls.size(); i++) t_row((i ? L"  " : L"> ") + ls[i], true);
    }
    void print_error(const std::string& msg) {
        begin(K_ERR);
        t_row(clip(L"! " + flat(to_cells(msg)), W()), false);
    }
    std::wstring tool_line(const std::wstring& left, const std::wstring& t) {
        int w = W();
        if (t.empty()) return clip(left, w);
        std::wstring s = clip(left, w - (int)t.size() - 1);
        s.resize((size_t)(w - (int)t.size()), L' ');
        return s + t;
    }
    std::wstring tool_left(const std::string& name, const Json& args) {
        return L"  * " + flat(to_cells(name)) + L"  " + flat(to_cells(short_args(name, args)));
    }

    void set_title() { scr_.title(ctx_ + (ag_.yolo() ? L"  yolo" : L"")); }
    void clear_screen() {
        scr_.clear();
        trow_ = 0;
        open_ = false;
        line_.clear();
        last_ = K_NONE;
        live_.clear();
        reset_text_state();
    }
    void replay(const std::string& session_id);

    // events and input
    void on_event(const Json& ev);
    void read_input();
    void key(const KEY_EVENT_RECORD& k, bool newline_hint);
    bool late_keydown();
    void submit();
    void command(const std::string& cmd);
    void cancel() {
        ag_.cancel();
        cancel_sent_ = true;
    }
    void tick();
    void render();
    void live_rows(std::vector<std::pair<std::wstring, bool>>& L, int& crow, int& ccol, bool& vis);
    void editor_rows(std::vector<std::pair<std::wstring, bool>>& L, int& crow, int& ccol);
};

// ---------------------------------------------------------------- streaming text

void UI::text_char(wchar_t c) {
    int w = W();
    if (c == L'\r') return;
    if (c == L'\t') {
        for (int i = 0; i < 4; i++) text_char(L' ');
        return;
    }
    if (c == L'\n') {
        if (in_text_) pend_nl_++;
        else lead_ = 0;
        return;
    }
    if (!in_text_) {
        if (c == L' ') { lead_++; return; }
        thinking_ = false;
        int lead = lead_;
        text_mark_ = mark();
        begin(K_TEXT);
        in_text_ = true;
        open_line();
        para_start_ = true;
        para_spaces_ = 0;
        indent_ = 0;
        row_indent_ = 0;
        in_word_ = false;
        for (int i = 0; i < lead && i < w / 2; i++) text_char(L' ');
    }
    if (pend_nl_) {
        close_line();
        if (pend_nl_ >= 2) t_blank();
        pend_nl_ = 0;
        open_line();
        para_start_ = true;
        para_spaces_ = 0;
        indent_ = 0;
        row_indent_ = 0;
        in_word_ = false;
    }
    if (c == L' ') {
        if (para_start_) para_spaces_++;
        if ((int)line_.size() < w) { line_ += L' '; dirty_ = true; }
        else new_row_indented();  // a space at the edge is the break
        in_word_ = false;
        return;
    }
    if (para_start_) {
        para_start_ = false;
        indent_ = std::min(para_spaces_, w / 2);
    }
    if (!in_word_) {
        in_word_ = true;
        word_start_ = line_.size();
    }
    if ((int)line_.size() < w) {
        line_ += c;
        dirty_ = true;
        return;
    }
    // The row is full: move the current word to the next row, or break it if it fills the row.
    std::wstring word;
    if ((int)word_start_ > row_indent_) {
        word = line_.substr(word_start_);
        line_.resize(word_start_);
        dirty_ = true;
    }
    new_row_indented();
    if ((int)(line_.size() + word.size()) + 1 > w) line_.clear(), row_indent_ = 0;
    word_start_ = line_.size();
    line_ += word;
    line_ += c;
    dirty_ = true;
}

void UI::add_reasoning(const std::string& utf8) {
    std::wstring c = to_cells(utf8);
    for (wchar_t ch : c) {
        bool sp = ch == L' ' || ch == L'\n' || ch == L'\t';
        if (sp) {
            if (!think_.empty() && think_.back() != L' ') think_ += L' ';
        } else {
            think_ += ch;
        }
    }
    if (think_.size() > 1200) think_.erase(0, think_.size() - 600);
    thinking_ = true;
}

// Content deltas. A reply that starts with <think> (reasoning inlined by the server) goes to the
// ticker until </think>.
void UI::on_text(const std::string& t) {
    if (tag_state_ == 2) { text_cells(to_cells(t)); return; }
    tag_buf_ += t;
    if (tag_state_ == 0) {
        size_t p = tag_buf_.find_first_not_of(" \t\r\n");
        if (p == std::string::npos) return;
        static const std::string open = "<think>";
        std::string rest = tag_buf_.substr(p);
        size_t n = std::min(rest.size(), open.size());
        if (rest.compare(0, n, open, 0, n) != 0) {
            tag_state_ = 2;
            std::string all;
            all.swap(tag_buf_);
            text_cells(to_cells(all));
            return;
        }
        if (rest.size() < open.size()) return;
        tag_state_ = 1;
        tag_buf_ = rest.substr(open.size());
    }
    size_t e = tag_buf_.find("</think>");
    if (e == std::string::npos) {
        size_t cut = utf8_cut(tag_buf_, tag_buf_.size() > 8 ? tag_buf_.size() - 8 : 0);
        if (cut) add_reasoning(tag_buf_.substr(0, cut));
        tag_buf_.erase(0, cut);
        return;
    }
    add_reasoning(tag_buf_.substr(0, e));
    std::string rest = tag_buf_.substr(e + 8);
    tag_buf_.clear();
    tag_state_ = 2;
    thinking_ = false;
    think_.clear();
    text_cells(to_cells(rest));
}

// ---------------------------------------------------------------- replay (resume / undo)

static std::string strip_think(const std::string& s) {
    size_t p = s.find_first_not_of(" \t\r\n");
    if (p == std::string::npos || s.compare(p, 7, "<think>") != 0) return s;
    size_t e = s.find("</think>", p);
    return e == std::string::npos ? std::string() : s.substr(e + 8);
}

void UI::replay(const std::string& id) {
    if (id.empty()) return;
    std::string data;
    if (!read_file(path_join(ag_.config().sessions_dir(), id + ".jsonl"), data)) return;
    std::vector<Json> recs;
    std::set<long long> undone;
    for (auto& l : split_lines(data)) {
        Json j;
        if (!Json::parse(l, j) || !j.is_object()) continue;
        if (j.get_str("t") == "undo") undone.insert(j.get_int("turn", -1));
        recs.push_back(std::move(j));
    }
    std::map<std::string, std::pair<std::string, std::string>> calls;  // id -> (name, arguments)
    std::set<long long> err_turns;
    std::map<std::string, std::pair<std::string, long long>> asked;  // request_id -> (tool_call_id, ts)
    std::map<std::string, long long> waited;                          // tool_call_id -> ms waited for approval
    for (auto& j : recs) {
        std::string t = j.get_str("t");
        long long turn = j.get_int("turn", -1);
        if (undone.count(turn)) continue;
        if (t == "msg") {
            const Json& m = j["msg"];
            std::string role = m.get_str("role");
            if (role == "user") {
                if (!m.get_bool("is_summary")) print_user(m.get_str("content"));
            } else if (role == "assistant") {
                std::string c = strip_think(m.get_str("content"));
                static const std::string mark = "\n[cancelled by the user]";
                if (m.get_bool("is_cancelled_partial") && ends_with(c, mark)) c.resize(c.size() - mark.size());
                text_cells(to_cells(c));
                text_end();
                const Json& tcs = m["tool_calls"];
                for (size_t i = 0; i < tcs.size(); i++) {
                    const Json& f = tcs.at(i)["function"];
                    calls[tcs.at(i).get_str("id")] = std::make_pair(f.get_str("name"), f.get_str("arguments"));
                }
            }
        } else if (t == "event") {
            const Json& e = j["ev"];
            std::string et = e.get_str("type");
            if (et == "approval_request") {
                asked[e.get_str("request_id")] = std::make_pair(e.get_str("tool_call_id"), iso_ms(j.get_str("ts")));
            } else if (et == "approval_resolved") {
                auto a = asked.find(e.get_str("request_id"));
                if (a != asked.end()) waited[a->second.first] = iso_ms(j.get_str("ts")) - a->second.second;
            } else if (et == "tool_finished") {
                if (e.get_str("summary") == "not run (cancelled)") continue;
                std::string cid = e.get_str("id"), name = e.get_str("name"), raw;
                auto it = calls.find(cid);
                if (it != calls.end()) raw = it->second.second;
                Json args;
                if (!Json::parse(raw.empty() ? std::string("{}") : raw, args)) args = Json(raw);
                if (args.is_string()) { Json inner; if (Json::parse(args.as_string(), inner)) args = inner; }
                begin(K_TOOL);
                long long ms = e.get_int("duration_ms", 0) - (waited.count(cid) ? waited[cid] : 0);
                bool denied = e.get_bool("denied"), cancelled = e.get_bool("cancelled");
                t_row(tool_line(tool_left(name, args), ms >= 5000 && !denied ? dur((unsigned long)ms) : L""), false);
                if (denied && !cancelled) t_row(L"    -> denied", false);
                else if (cancelled && !denied && name == "bash") t_row(L"    -> killed", false);
            } else if (et == "error" && e.get_bool("fatal")) {
                print_error(short_error(e.get_str("message")));
                err_turns.insert(turn);
            } else if (et == "turn_finished" && e.get_str("status") == "error" && !e.get_str("error").empty() &&
                       !err_turns.count(turn)) {
                print_error(short_error(e.get_str("error")));
            }
        }
    }
    text_end();
    close_line();
}

// ---------------------------------------------------------------- backend events

void UI::on_event(const Json& ev) {
    std::string t = ev.get_str("type");
    // A request got through: the endpoint line goes away.
    if (unreach_ && (t == "text_delta" || t == "reasoning_delta" || t == "tool_call_started" || t == "usage" ||
                     (t == "turn_finished" && ev.get_str("status") != "error"))) {
        rewind(unreach_mark_);
        unreach_ = false;
    }
    if (t == "reasoning_delta") {
        add_reasoning(ev.get_str("text"));
    } else if (t == "text_delta") {
        on_text(ev.get_str("text"));
    } else if (t == "tool_call_started") {
        thinking_ = false;
        think_.clear();
        text_mark_.valid = false;
        std::string id = ev.get_str("id");
        if (cancel_sent_) { skip_ids_.insert(id); return; }  // calls after a cancel never run
        begin(K_TOOL);
        tool_ = Tool();
        tool_.active = true;
        tool_.started = true;
        tool_.id = id;
        tool_.name = ev.get_str("name");
        tool_.row = trow_;
        tool_.left = tool_left(tool_.name, ev["args"]);
        tool_.t0 = tick_ms();
        t_row(tool_line(tool_.left, L""), false);
    } else if (t == "approval_request") {
        thinking_ = false;
        approval_ = ev.get_str("request_id");
        if (tool_.active) tool_.started = false;
    } else if (t == "approval_resolved") {
        approval_.clear();
        if (tool_.active && ev.get_bool("approved")) {
            tool_.started = true;
            tool_.t0 = tick_ms();
        }
    } else if (t == "tool_finished") {
        tag_state_ = 0;
        tag_buf_.clear();
        std::string id = ev.get_str("id");
        if (skip_ids_.count(id) || !tool_.active || tool_.id != id) return;
        unsigned long el = tool_.started ? tick_ms() - tool_.t0 : 0;
        bool denied = ev.get_bool("denied"), cancelled = ev.get_bool("cancelled");
        if (el >= 5000) scr_.row(tool_.row, tool_line(tool_.left, dur(el)), false);
        if (denied && !cancelled) t_row(L"    -> denied", false);
        else if (cancelled && !denied && tool_.name == "bash") t_row(L"    -> killed", false);
        tool_ = Tool();
    } else if (t == "usage") {
        long long ctx = ev.get_int("context_length", 0);
        long long used = ev.get_int("total_tokens", 0);
        if (used <= 0) used = ev.get_int("prompt_tokens", 0) + ev.get_int("completion_tokens", 0);
        if (ctx > 0) {
            ctx_ = L"ctx " + wnum((used * 100 + ctx / 2) / ctx) + L"%";
            set_title();
        }
    } else if (t == "retry") {
        thinking_ = false;
        think_.clear();
        if (ev.get_bool("discard_partial") && text_mark_.valid) rewind(text_mark_);
        reset_text_state();
        bool down = unreachable_error(ev.get_str("error"));
        if (unreach_ && !down) {  // the endpoint answered (with an error worth retrying)
            rewind(unreach_mark_);
            unreach_ = false;
        }
        if (!unreach_ && down) {
            unreach_mark_ = mark();
            begin(K_ERR);
            t_row(L"! can't reach endpoint", false);
            unreach_ = true;
        }
    } else if (t == "error") {
        if (!ev.get_bool("fatal")) return;
        std::string m = short_error(ev.get_str("message"));
        err_printed_ = true;
        if (unreach_ && m == "can't reach endpoint") { unreach_ = false; return; }  // the line stays
        if (unreach_) {
            rewind(unreach_mark_);
            unreach_ = false;
        }
        thinking_ = false;
        print_error(m);
    } else if (t == "turn_finished") {
        thinking_ = false;
        think_.clear();
        approval_.clear();
        text_end();
        if (unreach_) {  // cancelled while retrying
            rewind(unreach_mark_);
            unreach_ = false;
        }
        if (ev.get_str("status") == "error" && !err_printed_ && !ev.get_str("error").empty())
            print_error(short_error(ev.get_str("error")));
        busy_ = false;
        tool_ = Tool();
        reset_text_state();
    }
}

// ---------------------------------------------------------------- input

bool UI::late_keydown() {
    // A paste can still be arriving: look for more key presses for a moment.
    for (int i = 0; i < 4; i++) {
        Sleep(10);
        INPUT_RECORD r[64];
        DWORD n = 0;
        if (!PeekConsoleInputW(in_, r, 64, &n)) return false;
        for (DWORD k = 0; k < n; k++)
            if (r[k].EventType == KEY_EVENT && r[k].Event.KeyEvent.bKeyDown) return true;
    }
    return false;
}

void UI::read_input() {
    DWORD n = 0;
    if (!GetNumberOfConsoleInputEvents(in_, &n)) { quit_ = true; return; }
    if (n == 0) return;
    std::vector<INPUT_RECORD> recs(n);
    DWORD got = 0;
    if (!ReadConsoleInputW(in_, recs.data(), n, &got)) { quit_ = true; return; }
    int chars = 0;          // characters typed earlier in this burst
    bool prev_cr = false;   // the previous key was an Enter that produced '\r'
    for (DWORD i = 0; i < got && !quit_; i++) {
        if (recs[i].EventType != KEY_EVENT) continue;
        const KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
        if (!k.bKeyDown) {
            // Alt+numpad entry delivers its character on the Alt release.
            if (k.wVirtualKeyCode == VK_MENU && k.uChar.UnicodeChar >= 0x20) {
                KEY_EVENT_RECORD c = k;
                c.wVirtualKeyCode = 0;
                c.dwControlKeyState = 0;
                key(c, false);
            }
            continue;
        }
        if (k.wVirtualKeyCode == VK_RETURN) {
            if (prev_cr && k.uChar.UnicodeChar == L'\n') { prev_cr = false; continue; }  // CR LF from a paste
            bool more = false;
            for (DWORD j = i + 1; j < got && !more; j++)
                if (recs[j].EventType == KEY_EVENT && recs[j].Event.KeyEvent.bKeyDown) more = true;
            // A paste arrives as a burst of key events: an Enter with more keys queued behind it, or
            // after other characters of the same batch (a paste that ends with a newline), is a newline.
            bool hint = more || chars >= 2;
            if (!hint && !busy_ && !picker_ && !(k.dwControlKeyState & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
                hint = late_keydown();
            prev_cr = k.uChar.UnicodeChar == L'\r';
            int rep = k.wRepeatCount ? k.wRepeatCount : 1;
            for (int r = 0; r < rep; r++) key(k, hint);
            continue;
        }
        prev_cr = false;
        if (k.uChar.UnicodeChar >= 0x20) chars++;
        int rep = k.wRepeatCount ? k.wRepeatCount : 1;
        for (int r = 0; r < rep && !quit_; r++) key(k, false);
    }
}

void UI::key(const KEY_EVENT_RECORD& k, bool newline_hint) {
    WORD vk = k.wVirtualKeyCode;
    wchar_t ch = k.uChar.UnicodeChar;
    DWORD st = k.dwControlKeyState;
    bool ctrl = (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    bool alt = (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
    bool shift = (st & SHIFT_PRESSED) != 0;

    if (ch == 3 || (vk == 'C' && ctrl && !alt)) {  // Ctrl+C
        if (busy_) cancel();
        else quit_ = true;
        return;
    }
    if (vk == VK_ESCAPE) {
        if (picker_) picker_ = false;
        else if (busy_) cancel();
        return;
    }
    if (picker_) {
        int n = (int)items_.size();
        if (vk == VK_UP) sel_ = (sel_ + n - 1) % n;
        else if (vk == VK_DOWN) sel_ = (sel_ + 1) % n;
        else if (vk == VK_PRIOR) sel_ = std::max(0, sel_ - 10);
        else if (vk == VK_NEXT) sel_ = std::min(n - 1, sel_ + 10);
        else if (vk == VK_HOME) sel_ = 0;
        else if (vk == VK_END) sel_ = n - 1;
        else if (vk == VK_RETURN) {
            picker_ = false;
            std::string id = items_[(size_t)sel_].id, err;
            if (ag_.resume_session(id, &err)) {
                clear_screen();
                replay(ag_.session_id());
                ctx_ = L"ctx ?";
                set_title();
            } else {
                print_error(err);
            }
        }
        return;
    }
    if (busy_) {
        if (!approval_.empty() && (ch == L'y' || ch == L'Y' || ch == L'n' || ch == L'N')) {
            ag_.answer_approval(approval_, ch == L'y' || ch == L'Y');
            approval_.clear();
        }
        return;  // everything else is discarded while busy
    }
    switch (vk) {
    case VK_LEFT: if (caret_ > 0) caret_--; return;
    case VK_RIGHT: if (caret_ < ed_.size()) caret_++; return;
    case VK_HOME: while (caret_ > 0 && ed_[caret_ - 1] != L'\n') caret_--; return;
    case VK_END: while (caret_ < ed_.size() && ed_[caret_] != L'\n') caret_++; return;
    case VK_BACK: if (caret_ > 0) ed_.erase(--caret_, 1); return;
    case VK_DELETE: if (caret_ < ed_.size()) ed_.erase(caret_, 1); return;
    case VK_RETURN:
        if (shift || ctrl || newline_hint) ed_.insert(caret_++, 1, L'\n');
        else submit();
        return;
    }
    if (ch >= 0x20 && ch != 0x7F && !(ctrl && !alt)) ed_.insert(caret_++, 1, ch);
}

void UI::command(const std::string& cmd) {
    std::string err;
    if (cmd == "/help") {
        begin(K_SYS);
        t_row(kHelp, false);
    } else if (cmd == "/new") {
        ag_.close_session(&err);
        clear_screen();
        ctx_ = L"ctx 0%";
        set_title();
    } else if (cmd == "/clear") {
        clear_screen();
    } else if (cmd == "/yolo") {
        ag_.set_yolo(!ag_.yolo());
        set_title();
    } else if (cmd == "/undo") {
        Json rep;
        if (ag_.undo_last_turn(&rep, &err)) {
            clear_screen();
            replay(ag_.session_id());
        }
    } else if (cmd == "/resume") {
        items_.clear();
        for (auto& s : ag_.list_sessions())
            if (live_title(s.path, s.title) && !trim(s.title).empty()) items_.push_back(s);
        std::stable_sort(items_.begin(), items_.end(),
                         [](const SessionInfo& a, const SessionInfo& b) { return a.updated > b.updated; });
        if (!items_.empty()) {
            picker_ = true;
            sel_ = 0;
            top_ = 0;
        }
    }
    // anything else: nothing
}

void UI::submit() {
    std::wstring text = ed_;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L' ' || text.back() == L'\t')) text.pop_back();
    std::string utf8 = narrow(text);
    std::string tr = trim(utf8);
    if (tr.empty()) return;
    // "/word" is a command; "/c/path ..." and the like go to the model.
    if (tr[0] == '/') {
        size_t e = 1;
        while (e < tr.size() && isalpha((unsigned char)tr[e])) e++;
        if (e > 1 && (e == tr.size() || isspace((unsigned char)tr[e]))) {
            ed_.clear();
            caret_ = 0;
            command(lower(tr.substr(0, e)));
            return;
        }
    }
    std::string err;
    if (!ag_.start_turn(utf8, &err)) {
        print_error(err);
        return;
    }
    ed_.clear();
    caret_ = 0;
    busy_ = true;
    cancel_sent_ = false;
    err_printed_ = false;
    approval_.clear();
    thinking_ = false;
    think_.clear();
    skip_ids_.clear();
    tool_ = Tool();
    unreach_ = false;
    reset_text_state();
    print_user(utf8);
}

// ---------------------------------------------------------------- rendering

void UI::tick() {
    if (!tool_.active || !tool_.started) return;
    unsigned long el = tick_ms() - tool_.t0;
    if (el < 5000) return;
    std::wstring d = dur(el);
    if (d == tool_.shown) return;
    tool_.shown = d;
    scr_.row(tool_.row, tool_line(tool_.left, d), false);
}

static wchar_t cell_of(wchar_t ch) {
    if (ch >= 0x20 && ch < 0x7F) return ch;
    if (ch == L'\t') return L' ';
    if (ch >= 0xD800 && ch <= 0xDFFF) return L'*';
    std::wstring c = to_cells(narrow(std::wstring(1, ch)));
    return c.size() == 1 ? c[0] : L'?';
}

void UI::editor_rows(std::vector<std::pair<std::wstring, bool>>& L, int& crow, int& ccol) {
    int wc = std::max(1, W() - 2);
    std::wstring line;
    int cur = -1;
    bool first = true;
    auto push = [&]() {
        L.push_back(std::make_pair((first ? L"> " : L"  ") + line, false));
        if (cur >= 0) { crow = (int)L.size() - 1; ccol = cur + 2; }
        first = false;
        line.clear();
        cur = -1;
    };
    for (size_t i = 0; i <= ed_.size(); i++) {
        if (i == caret_) {
            if ((int)line.size() == wc) push();
            cur = (int)line.size();
        }
        if (i == ed_.size()) break;
        wchar_t ch = ed_[i];
        if (ch == L'\n') { push(); continue; }
        if ((int)line.size() == wc) push();
        line += cell_of(ch);
    }
    push();
}

void UI::live_rows(std::vector<std::pair<std::wstring, bool>>& L, int& crow, int& ccol, bool& vis) {
    int w = W();
    crow = 0;
    ccol = 0;
    vis = false;
    if (picker_) {
        int n = (int)items_.size();
        int show = std::max(1, std::min(n, scr_.win_height() - 2));
        if (sel_ < top_) top_ = sel_;
        if (sel_ >= top_ + show) top_ = sel_ - show + 1;
        if (top_ > n - show) top_ = n - show;
        if (top_ < 0) top_ = 0;
        long long now = now_secs();
        for (int i = top_; i < top_ + show && i < n; i++) {
            const SessionInfo& s = items_[(size_t)i];
            std::wstring a = age(now - iso_secs(s.updated));
            int room = w - (int)a.size() - 4;
            std::wstring t = clip(flat(to_cells(s.title)), room);
            t.resize((size_t)std::max(room, 0), L' ');
            L.push_back(std::make_pair(L"  " + t + L"  " + a, i == sel_));
            if (i == sel_) crow = (int)L.size() - 1;
        }
        return;
    }
    if (busy_) {
        if (!approval_.empty()) {
            L.push_back(std::make_pair(std::wstring(L"allow? y/n "), false));
            ccol = 11;
            vis = true;
        } else if (thinking_ && !think_.empty()) {
            int room = w - 2;
            std::wstring tail = think_;
            while (!tail.empty() && tail.back() == L' ') tail.pop_back();
            if ((int)tail.size() > room) {
                tail = tail.substr(tail.size() - (size_t)room);
                size_t sp = tail.find(L' ');
                if (sp != std::wstring::npos) tail = tail.substr(sp + 1);
            }
            L.push_back(std::make_pair(L"~ " + tail, false));
        }
        return;
    }
    editor_rows(L, crow, ccol);
    vis = true;
}

void UI::render() {
    flush();
    if (!scr_.console()) {
        long long upto = trow_;
        if (tool_.active) upto = std::min(upto, tool_.row);
        if (unreach_) upto = std::min(upto, unreach_mark_.row);
        if (text_mark_.valid && busy_) upto = std::min(upto, text_mark_.row);
        scr_.commit(upto);
        return;
    }
    std::vector<std::pair<std::wstring, bool>> L;
    int crow = 0, ccol = 0;
    bool vis = false;
    live_rows(L, crow, ccol, vis);
    long long nf = next_free();
    long long start = nf > 0 ? nf + 1 : 0;
    int w = W();
    for (size_t i = 0; i < L.size(); i++) {
        long long v = start + (long long)i;
        L[i].first.resize((size_t)w, L' ');
        auto it = live_.find(v);
        if (it != live_.end() && it->second == L[i]) continue;
        scr_.row(v, L[i].first, L[i].second);
        live_[v] = L[i];
    }
    for (auto it = live_.begin(); it != live_.end();) {
        long long v = it->first;
        if (v >= start && v < start + (long long)L.size()) { ++it; continue; }
        if (v >= nf) scr_.row(v, std::wstring(), false);
        it = live_.erase(it);
    }
    if (L.empty()) scr_.cursor(nf, 0, false);
    else scr_.cursor(start + (picker_ ? (long long)L.size() - 1 : crow), ccol, vis);
}

int UI::run() {
    in_ = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (!GetConsoleMode(in_, &in_mode)) {
        std::string m = "! the interactive mode needs a console (use the run or stdio commands otherwise)\r\n";
        DWORD n;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), m.data(), (DWORD)m.size(), &n, nullptr);
        return 64;
    }
    // Ctrl+C becomes a key event; no line editing or echo by the console.
    SetConsoleMode(in_, in_mode & ~(DWORD)(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                         ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT));
    FlushConsoleInputBuffer(in_);
    wchar_t old_title[512] = {0};
    GetConsoleTitleW(old_title, 512);
    scr_.init();
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ui_ctrl_handler, TRUE);

    clear_screen();
    set_title();
    while (!quit_) {
        scr_.refresh();
        DWORD w = WaitForSingleObject(in_, 50);
        if (g_close) break;
        if (w == WAIT_OBJECT_0) read_input();
        if (quit_ || g_close) break;
        Json ev;
        for (int n = 0; n < 2000 && ag_.next_event(ev, 0); n++) on_event(ev);
        tick();
        render();
    }

    // End the conversation properly (kills background processes, deletes the scratchpad).
    bool closing = g_close != 0;
    if (!closing) {
        for (auto& r : live_)
            if (r.first >= next_free()) scr_.row(r.first, std::wstring(), false);
        live_.clear();
        text_end();
        close_line();
        scr_.commit(trow_);
        scr_.cursor(next_free(), 0, true);
    }
    ag_.cancel();
    ag_.shutdown();
    if (!closing) {
        scr_.restore();
        SetConsoleMode(in_, in_mode);
        SetConsoleTitleW(old_title);
    }
    SetEvent(g_done);
    return 0;
}

}  // namespace

int run_ui(const Config& cfg) {
    Agent agent;
    std::string err;
    if (!agent.start(cfg, &err)) {
        log_error("startup: " + err);
        std::wstring m = L"! " + to_cells(err) + L"\r\n";
        DWORD n;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!WriteConsoleW(out, m.data(), (DWORD)m.size(), &n, nullptr)) {
            std::string u = narrow(m);
            WriteFile(out, u.data(), (DWORD)u.size(), &n, nullptr);
        }
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        if (GetConsoleMode(in, &mode)) {  // keep a double-clicked window open long enough to read it
            FlushConsoleInputBuffer(in);
            INPUT_RECORD r;
            DWORD got;
            while (ReadConsoleInputW(in, &r, 1, &got) && !(r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown)) {}
        }
        return 1;
    }
    UI ui(agent);
    return ui.run();
}

}  // namespace ag
