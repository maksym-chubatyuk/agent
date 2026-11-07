// agent: interactive console frontend (default, see ui.cpp), plus the headless test driver and the
// JSON-lines-over-stdio server for the backend. In the driver modes every output line is one JSON
// object, ASCII-escaped (\uXXXX) so the CP437 console can't mangle it.
#include "agent.h"
#include "console.h"
#include "ui.h"
#include "util.h"
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace ag;

static const char* kVersion = "agent 1.0";

static Mutex g_out_mu;
static void out_line(const Json& j) {
    std::string s = j.dump(true);
    s += "\n";
    Lock l(g_out_mu);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
}

static Json obj(const char* type) {
    Json j = Json::object();
    j.set("type", type);
    return j;
}

// Read one line from stdin (blocking). Returns false on EOF.
static bool read_stdin_line(std::string& line) {
    static std::string buf;
    static bool eof = false;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    while (true) {
        size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            buf.erase(0, nl + 1);
            return true;
        }
        if (eof) {
            if (buf.empty()) return false;
            line.swap(buf);
            buf.clear();
            return true;
        }
        char tmp[4096];
        DWORD got = 0;
        if (!ReadFile(h, tmp, sizeof tmp, &got, nullptr) || got == 0) { eof = true; continue; }
        buf.append(tmp, got);
    }
}

static std::string read_stdin_all() {
    std::string all, line;
    bool first = true;
    while (read_stdin_line(line)) {
        if (!first) all += "\n";
        all += line;
        first = false;
    }
    return all;
}

static Json session_list_json(Agent& a) {
    Json arr = Json::array();
    for (auto& s : a.list_sessions()) {
        Json j = Json::object();
        j.set("id", s.id).set("created", s.created).set("updated", s.updated).set("title", s.title).set("bytes", s.bytes);
        arr.push_back(j);
    }
    return arr;
}

static Json stats_json(long long tool_calls) {
    ProcStats ps = proc_stats();
    Json j = obj("stats");
    j.set("threads", ps.threads).set("reserved_kb", (long long)ps.reserved_kb).set("committed_kb", (long long)ps.committed_kb)
        .set("largest_free_kb", (long long)ps.largest_free_kb).set("tool_calls", tool_calls);
    return j;
}

static Agent* g_agent = nullptr;
static volatile LONG g_ctrl_c = 0;
static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        if (InterlockedIncrement(&g_ctrl_c) == 1 && g_agent) { g_agent->cancel(); return TRUE; }
    }
    return FALSE;
}

static void usage() {
    out_line(obj("usage").set("text",
        "agent [--config FILE] [--set section.key=value]... [--yolo] [COMMAND]\n"
        "  (no command)                   interactive console UI\n"
        "  run [--session ID|last] [--yolo] [--approve-stdin] [--cancel-after SEC] [--stats] [--brief] PROMPT|-\n"
        "  list\n"
        "  undo --session ID|last\n"
        "  history --session ID|last\n"
        "  stdio [--session ID|last]      JSON-lines server (commands on stdin, events on stdout)\n"
        "  version"));
}

// ---------------------------------------------------------------- stdio server

static int run_stdio(Agent& agent) {
    out_line(obj("ready").set("version", kVersion).set("session", agent.session_id()).set("yolo", agent.yolo()));
    std::string line;
    while (read_stdin_line(line)) {
        if (trim(line).empty()) continue;
        Json cmd;
        std::string perr;
        Json res = obj("result");
        if (!Json::parse(line, cmd, &perr) || !cmd.is_object()) {
            res.set("ok", false).set("error", "bad JSON command: " + perr);
            out_line(res);
            continue;
        }
        std::string c = cmd.get_str("cmd");
        res.set("cmd", c);
        if (cmd.has("id")) res.set("id", cmd["id"]);
        std::string err;
        bool ok = true;
        if (c == "turn") {
            ok = agent.start_turn(cmd.get_str("text"), &err);
            res.set("session", agent.session_id());
        } else if (c == "cancel") {
            agent.cancel();
        } else if (c == "approve") {
            ok = agent.answer_approval(cmd.get_str("request_id"), cmd.get_bool("approve", false));
            if (!ok) err = "no pending approval with that request_id";
        } else if (c == "new") {
            std::string id;
            ok = agent.new_session(&id, &err);
            res.set("session", id);
        } else if (c == "close") {
            ok = agent.close_session(&err);
        } else if (c == "list") {
            res.set("sessions", session_list_json(agent));
        } else if (c == "resume") {
            ok = agent.resume_session(cmd.get_str("session", "last"), &err);
            if (ok) res.set("session", agent.session_id()).set("history", agent.history());
        } else if (c == "undo") {
            Json rep;
            ok = agent.undo_last_turn(&rep, &err);
            if (ok) res.set("undo", rep);
        } else if (c == "history") {
            if (agent.busy()) { ok = false; err = "busy"; }
            else res.set("session", agent.session_id()).set("history", agent.history());
        } else if (c == "set_yolo") {
            agent.set_yolo(cmd.get_bool("value", false));
            res.set("yolo", agent.yolo());
        } else if (c == "status") {
            res.set("busy", agent.busy()).set("session", agent.session_id()).set("yolo", agent.yolo());
        } else if (c == "stats") {
            Json s = stats_json(0);
            res.set("threads", s["threads"]).set("reserved_kb", s["reserved_kb"]).set("largest_free_kb", s["largest_free_kb"]);
        } else if (c == "wait") {
            // Block command processing until the agent needs input: the running turn (if any) has
            // finished, or it waits for an approval. Handy for scripted clients that write all
            // commands at once. Events keep flowing meanwhile.
            while (agent.busy() && agent.pending_approval().empty()) Sleep(100);
            res.set("busy", agent.busy()).set("pending_approval", agent.pending_approval());
        } else if (c == "quit") {
            res.set("ok", true);
            out_line(res);
            break;
        } else {
            ok = false;
            err = "unknown cmd '" + c + "'";
        }
        res.set("ok", ok);
        if (!ok) res.set("error", err);
        out_line(res);
    }
    agent.cancel();
    agent.shutdown();
    return 0;
}

// ---------------------------------------------------------------- main

int main() {
    install_crash_handler();
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(narrow(wargv[i]));
    LocalFree(wargv);

    std::string config_path = path_join(exe_dir(), "agent.ini");
    std::map<std::string, std::string> overrides;
    std::string command, session, prompt;
    bool yolo = false, approve_stdin = false, stats = false, have_prompt = false, brief = false;
    double cancel_after = -1;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= args.size()) { out_line(obj("error").set("message", "missing value for " + a)); return false; }
            dst = args[++i];
            return true;
        };
        std::string v;
        if (a == "--config") { if (!next(config_path)) return 64; }
        else if (a == "--set") {
            if (!next(v)) return 64;
            size_t eq = v.find('=');
            if (eq == std::string::npos) { out_line(obj("error").set("message", "--set needs section.key=value")); return 64; }
            overrides[trim(v.substr(0, eq))] = trim(v.substr(eq + 1));
        }
        else if (a == "--session" || a == "--resume") { if (!next(session)) return 64; }
        else if (a == "--yolo") yolo = true;
        else if (a == "--approve-stdin") approve_stdin = true;
        else if (a == "--stats") stats = true;
        else if (a == "--brief") brief = true;
        else if (a == "--cancel-after") { if (!next(v)) return 64; cancel_after = atof(v.c_str()); }
        else if (a == "--version" || a == "version") { command = "version"; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (command.empty()) command = a;
        else if (!have_prompt) { prompt = a; have_prompt = true; }
        else { prompt += " " + a; }
    }
    if (command == "version") { out_line(obj("version").set("version", kVersion)); return 0; }
    bool ui = command.empty();
    // Interactive mode reports a startup problem as one readable line and waits for a key, so a
    // double-clicked window doesn't vanish before it can be read.
    auto fatal = [&](const std::string& msg) {
        if (!ui) { out_line(obj("error").set("message", msg).set("fatal", true)); return; }
        std::wstring m = L"! " + to_cells(msg) + L"\r\n";
        DWORD n;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), in = GetStdHandle(STD_INPUT_HANDLE);
        if (!WriteConsoleW(out, m.data(), (DWORD)m.size(), &n, nullptr)) {
            std::string u = narrow(m);
            WriteFile(out, u.data(), (DWORD)u.size(), &n, nullptr);
        }
        DWORD mode;
        if (GetConsoleMode(in, &mode)) {
            FlushConsoleInputBuffer(in);
            INPUT_RECORD r;
            DWORD got;
            while (ReadConsoleInputW(in, &r, 1, &got) && !(r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown)) {}
        }
    };

    Config cfg;
    std::string err;
    if (!file_exists(config_path) && config_path != path_join(exe_dir(), "agent.ini")) {
        log_error("config file not found: " + config_path);
        fatal("config file not found: " + config_path);
        return 64;
    }
    if (!cfg.load(file_exists(config_path) ? config_path : std::string(), &err, &overrides)) {
        log_error("config: " + err);
        fatal(err);
        return 64;
    }
    if (yolo) cfg.yolo = true;
    if (ui) return run_ui(cfg);

    Agent agent;
    g_agent = &agent;
    // stdio mode: events are written as they happen, from the worker thread (out_line is locked).
    if (command == "stdio") agent.set_event_callback([](const Json& ev) { out_line(ev); });
    if (!agent.start(cfg, &err)) {
        log_error("startup: " + err);
        out_line(obj("error").set("message", err).set("fatal", true));
        return 1;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (!session.empty() && command != "list") {
        if (!agent.resume_session(session, &err)) {
            out_line(obj("error").set("message", err).set("fatal", true));
            return 1;
        }
    }

    if (command == "list") {
        out_line(obj("sessions").set("sessions", session_list_json(agent)));
        return 0;
    }
    if (command == "history") {
        if (session.empty()) { out_line(obj("error").set("message", "history needs --session")); return 64; }
        out_line(obj("history").set("session", agent.session_id()).set("messages", agent.history()));
        return 0;
    }
    if (command == "undo") {
        if (session.empty()) { out_line(obj("error").set("message", "undo needs --session")); return 64; }
        Json rep;
        if (!agent.undo_last_turn(&rep, &err)) { out_line(obj("error").set("message", err)); return 1; }
        out_line(obj("undo").set("session", agent.session_id()).set("undo", rep));
        return 0;
    }
    if (command == "stdio") return run_stdio(agent);
    if (command != "run") { usage(); return 64; }

    if (!have_prompt || prompt == "-") {
        if (approve_stdin) { out_line(obj("error").set("message", "--approve-stdin needs the prompt as an argument")); return 64; }
        prompt = read_stdin_all();
    }
    if (trim(prompt).empty()) { out_line(obj("error").set("message", "empty prompt")); return 64; }

    if (!agent.start_turn(prompt, &err)) { out_line(obj("error").set("message", err).set("fatal", true)); return 1; }
    unsigned long t0 = tick_ms();
    bool cancel_sent = false;
    long long tools = 0;
    int code = 1;
    std::map<std::string, size_t> deltas;  // --brief: delta bytes since the last printed event
    if (stats) out_line(stats_json(0));
    while (true) {
        Json ev;
        if (!agent.next_event(ev, 200)) {
            if (cancel_after >= 0 && !cancel_sent && tick_ms() - t0 >= (unsigned long)(cancel_after * 1000)) {
                cancel_sent = true;
                out_line(obj("driver").set("note", "cancel requested").set("elapsed_ms", (long long)(tick_ms() - t0)));
                agent.cancel();
            }
            continue;
        }
        std::string t = ev.get_str("type");
        if (brief && (t == "text_delta" || t == "reasoning_delta")) {
            deltas[t] += ev.get_str("text").size();
        } else {
            if (brief) {
                for (auto& d : deltas)
                    if (d.second) out_line(obj("driver").set("note", d.first + " bytes").set("bytes", (long long)d.second));
                deltas.clear();
                if (ev.has("result") && ev.get_str("result").size() > 300)
                    ev.set("result", utf8_head(ev.get_str("result"), 300) + "...[" + std::to_string(ev.get_str("result").size()) + " bytes, cut by --brief]");
            }
            out_line(ev);
        }
        if (t == "approval_request") {
            bool ok = false;
            if (approve_stdin) {
                std::string ans;
                if (read_stdin_line(ans)) {
                    std::string l = lower(trim(ans));
                    ok = l == "y" || l == "yes" || l == "approve" || l == "a" || l == "1" || l == "true";
                }
            }
            out_line(obj("driver").set("note", ok ? "approving" : "denying").set("request_id", ev.get_str("request_id")));
            agent.answer_approval(ev.get_str("request_id"), ok);
        } else if (t == "tool_finished") {
            tools++;
            if (stats) out_line(stats_json(tools));
        } else if (t == "turn_finished") {
            std::string st = ev.get_str("status");
            code = st == "done" ? 0 : st == "cancelled" ? 2 : 1;
            break;
        }
    }
    if (stats) out_line(stats_json(tools));
    agent.shutdown();
    return code;
}
