#include "session.h"
#include "util.h"
#include <algorithm>
#include <map>

namespace ag {

static std::string session_file(const std::string& dir, const std::string& id) { return path_join(dir, id + ".jsonl"); }

bool Session::create(const std::string& sessions_dir, const Json& meta_extra, std::string* err) {
    if (!make_dirs(sessions_dir)) { if (err) *err = "cannot create " + sessions_dir; return false; }
    std::string base = now_stamp();
    std::string sid;
    for (int k = 0; k < 1000; k++) {
        char suf[16];
        snprintf(suf, sizeof suf, "-%03d", k);
        sid = base + suf;
        if (!file_exists(session_file(sessions_dir, sid))) break;
    }
    id = sid;
    path = session_file(sessions_dir, sid);
    messages = Json::array();
    last_turn = 0;
    live_turns.clear();
    cwd.clear();
    Json meta = Json::object();
    meta.set("t", "meta").set("v", 1).set("id", sid).set("created", now_iso());
    for (auto& m : meta_extra.members()) meta.set(m.first, m.second);
    if (!append(meta)) { if (err) *err = "cannot write " + path; path.clear(); return false; }
    return true;
}

bool Session::open(const std::string& sessions_dir, const std::string& sid, std::string* err) {
    std::string p = session_file(sessions_dir, sid);
    if (!file_exists(p)) { if (err) *err = "no such session: " + sid; return false; }
    id = sid;
    path = p;
    return replay(err);
}

bool Session::append(const Json& rec0) {
    if (path.empty()) return false;
    Json rec = rec0;
    if (!rec.has("ts")) rec.set("ts", now_iso());
    std::string line = rec.dump(false);
    line += '\n';
    return append_file(path, line);
}

int Session::begin_turn() {
    int t = ++last_turn;
    live_turns.push_back(t);
    Json r = Json::object();
    r.set("t", "turn_start").set("turn", t);
    append(r);
    return t;
}

void Session::add_message(int turn, const Json& msg) {
    messages.push_back(msg);
    Json r = Json::object();
    r.set("t", "msg").set("turn", turn).set("msg", msg);
    append(r);
}

void Session::add_event(int turn, const Json& ev) {
    Json r = Json::object();
    r.set("t", "event").set("turn", turn).set("ev", ev);
    append(r);
}

void Session::set_cwd(int turn, const std::string& c) {
    if (c == cwd) return;
    cwd = c;
    Json r = Json::object();
    r.set("t", "cwd").set("turn", turn).set("path", c);
    append(r);
}

static void do_compact(Json& messages, size_t cut, const Json& summary_msg) {
    if (cut > messages.size()) cut = messages.size();
    messages.erase_range(0, cut);
    messages.items().insert(messages.items().begin(), summary_msg);
}

void Session::apply_compaction(int turn, size_t cut, const Json& summary_msg, const Json& info) {
    do_compact(messages, cut, summary_msg);
    Json r = Json::object();
    r.set("t", "compact").set("turn", turn).set("cut", (long long)cut).set("summary_msg", summary_msg);
    for (auto& m : info.members()) r.set(m.first, m.second);
    append(r);
}

static bool read_records(const std::string& path, std::vector<Json>& recs, int& bad) {
    std::string text;
    if (!read_file(path, text)) return false;
    bad = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        start = nl + 1;
        if (trim(line).empty()) continue;
        Json j;
        if (Json::parse(line, j) && j.is_object()) recs.push_back(std::move(j));
        else bad++;
    }
    return true;
}

bool Session::replay(std::string* err) {
    std::vector<Json> recs;
    int bad = 0;
    if (!read_records(path, recs, bad)) { if (err) *err = "cannot read " + path; return false; }
    if (bad) log_error("session " + id + ": skipped " + std::to_string(bad) + " unparsable line(s)");
    std::set<long long> undone;
    last_turn = 0;
    for (auto& r : recs) {
        long long t = r.get_int("turn", 0);
        if (t > last_turn) last_turn = (int)t;
        if (r.get_str("t") == "undo") undone.insert(t);
    }
    messages = Json::array();
    live_turns.clear();
    cwd.clear();
    for (auto& r : recs) {
        std::string kind = r.get_str("t");
        long long t = r.get_int("turn", 0);
        if (undone.count(t)) continue;
        if (kind == "turn_start") live_turns.push_back((int)t);
        else if (kind == "msg") messages.push_back(r["msg"]);
        else if (kind == "cwd") cwd = r.get_str("path");
        else if (kind == "compact") do_compact(messages, (size_t)r.get_int("cut", 0), r["summary_msg"]);
    }
    return true;
}

bool Session::undo_last(Json& report, std::string* err) {
    if (live_turns.empty()) { if (err) *err = "nothing to undo"; return false; }
    int t = live_turns.back();
    Json r = Json::object();
    r.set("t", "undo").set("turn", t);
    if (!append(r)) { if (err) *err = "cannot write " + path; return false; }
    if (!replay(err)) return false;
    report = Json::object();
    report.set("turn", t).set("messages", (long long)messages.size());
    return true;
}

static std::string filetime_iso(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buf[40];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::vector<SessionInfo> Session::list(const std::string& sessions_dir) {
    std::vector<SessionInfo> out;
    for (auto& name : list_dir(sessions_dir, "*.jsonl")) {
        SessionInfo si;
        si.id = name.substr(0, name.size() - 6);
        si.path = path_join(sessions_dir, name);
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(widen(si.path).c_str(), GetFileExInfoStandard, &d)) {
            si.bytes = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
            si.updated = filetime_iso(d.ftLastWriteTime);
        }
        // read the head to find meta + first user message
        HANDLE h = CreateFileW(widen(si.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string head(32768, 0);
            DWORD got = 0;
            ReadFile(h, &head[0], (DWORD)head.size(), &got, nullptr);
            CloseHandle(h);
            head.resize(got);
            for (auto& line : split_lines(head)) {
                Json j;
                if (!Json::parse(line, j)) continue;
                std::string t = j.get_str("t");
                if (t == "meta") si.created = j.get_str("created");
                else if (t == "msg" && j["msg"].get_str("role") == "user") {
                    std::string c = j["msg"].get_str("content");
                    c = replace_all(replace_all(c, "\r", " "), "\n", " ");
                    si.title = utf8_head(c, 80);
                    break;
                }
            }
        }
        out.push_back(si);
    }
    std::sort(out.begin(), out.end(), [](const SessionInfo& a, const SessionInfo& b) { return a.id > b.id; });
    return out;
}

std::string Session::latest_id(const std::string& sessions_dir) {
    auto l = list(sessions_dir);
    return l.empty() ? std::string() : l.front().id;
}

}  // namespace ag
