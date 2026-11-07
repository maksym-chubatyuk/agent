// First-run setup: plain console prompts, then an agent.ini in agent.ini.template's format.
// Enter keeps the value shown in [brackets]; "-" clears it. The model can be chosen from the
// server's /models list with Up/Down + Enter (drawn like the chat's /resume picker).
#include "setup.h"
#include "console.h"
#include "http.h"
#include "json.h"
#include "util.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

namespace ag {

namespace {

// A copy of agent.ini.template (keep the two identical). Setup replaces the values of the keys it
// knows and keeps every comment.
const char* kTemplate = R"INI(; agent configuration. agent.exe reads the file given with --config FILE, else
; %APPDATA%\agent\agent.ini, else agent.ini next to agent.exe. "agent.exe setup" asks for the main
; values and writes %APPDATA%\agent\agent.ini, or the --config file; it also runs on the first start
; when no config file exists.
; Format: [section] headers, "key = value" lines, ';' or '#' comments (also after a value when
; preceded by a space). Values may be wrapped in double quotes. Relative paths are resolved
; against the folder of this file. Any key can be overridden on the command line with
; --set section.key=value.

[llm]
base_url = http://localhost:8080/v1         ; OpenAI-compatible endpoint (plain HTTP), required
model = your-model                          ; model name sent in every request, required
api_key =                                   ; optional; sent as "Authorization: Bearer <key>"
context_length = 32768                      ; model context in tokens
compress_threshold = 0.75                   ; compress history when the estimate exceeds this fraction
keep_recent = 0.35                          ; fraction of context_length kept verbatim after compression
chars_per_token = 3.5                       ; token estimate = bytes / chars_per_token until the server reports usage
stream_usage = true                         ; send stream_options.include_usage and calibrate estimates from usage
max_tokens = 0                              ; 0 = don't send max_tokens
temperature = -1                            ; < 0 = don't send temperature
max_retries = 6                             ; retries on connection errors, 5xx/429 and broken streams
connect_timeout = 10                        ; seconds
idle_timeout = 600                          ; seconds without any byte before a request is abandoned

[firecrawl]
url = http://localhost:3002                 ; self-hosted Firecrawl (no API key); empty = web tools disabled
scrape_timeout_ms = 60000
search_limit = 5

[tools]
bash =                                      ; path to Git Bash's bash.exe; none = no bash; empty = auto-detect
                                            ; (PATH, Program Files\Git, LocalAppData\Programs\Git, \PortableGit)
workdir =                                   ; initial working directory (default: <data_dir>\work)
path_extra =                                ; prepended to the (deliberately short) child PATH
spill_threshold = 16000                     ; bytes; longer bash output / pages are saved to a file
spill_head = 4000                           ; bytes of the head returned with spilled output
spill_tail = 1000                           ; bytes of the tail returned with spilled bash output
read_limit = 500                            ; default number of lines for read_file
read_max_bytes = 60000                      ; cap on one read_file result

[agent]
data_dir = data                             ; sessions, memory, persona, spill files, logs
persona_file = persona.md                   ; relative to data_dir; loaded into the system prompt
memory_file = MEMORY.md                     ; relative to data_dir; loaded every turn, agent-editable
approval_patterns_file = approval_patterns.txt  ; relative to data_dir; extra risky-command regexes
yolo = false                                ; true = run risky commands without asking
machine_note =                              ; optional line about this computer for the system prompt
)INI";

struct Abort {};  // input closed (Ctrl+C in the model list, or end of input)

HANDLE g_in = INVALID_HANDLE_VALUE, g_out = INVALID_HANDLE_VALUE;

void say(const std::string& s) {
    std::wstring w = to_cells(s);
    DWORD n;
    WriteConsoleW(g_out, w.data(), (DWORD)w.size(), &n, nullptr);
}

std::string read_line() {
    std::wstring line;
    wchar_t buf[512];
    while (line.empty() || line.back() != L'\n') {
        DWORD got = 0;
        if (!ReadConsoleW(g_in, buf, 512, &got, nullptr) || got == 0) throw Abort();
        line.append(buf, got);
    }
    return trim(narrow(line));
}

// "label [default]: " -> the answer; Enter keeps the default, "-" clears it.
std::string ask(const std::string& label, const std::string& def, const std::string& shown = "") {
    std::string d = shown.empty() ? def : shown;
    say(label + (d.empty() ? std::string() : " [" + d + "]") + ": ");
    std::string v = read_line();
    if (v.empty()) return def;
    if (v == "-") return "";
    return v;
}

std::string masked(const std::string& key) {
    if (key.empty()) return "";
    if (key.size() <= 4) return std::string(key.size(), '*');
    return std::string(8, '*') + key.substr(key.size() - 4);
}

// "host:port/v1" -> "http://host:port/v1", no trailing slash. "" if it isn't plain http.
std::string http_url(std::string u) {
    if (u.find("://") == std::string::npos) u = "http://" + u;
    if (!starts_with(lower(u), "http://")) return "";
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

HttpResult get(const std::string& url, const std::string& key) {
    HttpOptions opt;
    opt.connect_timeout_ms = 5000;
    opt.idle_timeout_ms = 10000;
    opt.max_collect = 4 << 20;
    std::vector<std::pair<std::string, std::string>> h = {{"Accept", "application/json"}};
    if (!key.empty()) h.push_back(std::make_pair(std::string("Authorization"), "Bearer " + key));
    HttpResult r;
    http_request("GET", url, h, "", opt, nullptr, r);
    return r;
}

// GET <base>/models (OpenAI format: {"data":[{"id":...}]}).
std::vector<std::string> list_models(const std::string& base, const std::string& key, std::string& err) {
    std::vector<std::string> out;
    HttpResult r = get(base + "/models", key);
    if (r.status == 0) { err = r.error.empty() ? "no response" : r.error; return out; }
    if (r.status != 200) { err = "HTTP " + std::to_string(r.status); return out; }
    Json j;
    if (Json::parse(r.body, j) && j.is_object())
        for (auto& m : j["data"].items()) {
            std::string id = m.get_str("id");
            if (!id.empty()) out.push_back(id);
        }
    if (out.empty()) err = "no model list in the reply";
    return out;
}

// Up/Down + Enter list under the prompt `label`; selected row inverted, as in the chat's /resume
// picker. Returns the chosen index, or -1 for Esc. The prompt and list rows are erased afterwards
// and the cursor is left at the start of the prompt row.
int pick(const std::string& label, const std::vector<std::string>& items, int sel) {
    say(label + ":");
    CONSOLE_SCREEN_BUFFER_INFO bi;
    GetConsoleScreenBufferInfo(g_out, &bi);
    WORD norm = bi.wAttributes & 0xFF;
    WORD inv = (WORD)(((norm & 0x0F) << 4) | ((norm & 0xF0) >> 4));
    if (inv == norm) inv = 0x70;
    int w = bi.dwSize.X;
    int n = (int)items.size();
    int show = std::max(1, std::min(n, bi.srWindow.Bottom - bi.srWindow.Top - 3));
    say(std::string((size_t)show, '\n'));  // room for the list (scrolls the buffer if needed)
    GetConsoleScreenBufferInfo(g_out, &bi);
    SHORT y0 = (SHORT)(bi.dwCursorPosition.Y - show + 1);  // first list row; the prompt is y0 - 1

    CONSOLE_CURSOR_INFO ci0, ci;
    GetConsoleCursorInfo(g_out, &ci0);
    ci = ci0;
    ci.bVisible = FALSE;
    SetConsoleCursorInfo(g_out, &ci);
    DWORD mode0 = 0;
    GetConsoleMode(g_in, &mode0);
    SetConsoleMode(g_in, 0);  // raw keys, Ctrl+C included

    auto put = [&](SHORT y, const std::wstring& text, WORD attr) {
        std::wstring s = text;
        s.resize((size_t)w, L' ');
        COORD c = {0, y};
        DWORD k;
        WriteConsoleOutputCharacterW(g_out, s.data(), (DWORD)s.size(), c, &k);
        FillConsoleOutputAttribute(g_out, attr, (DWORD)s.size(), c, &k);
    };
    int top = 0, result = -1;
    bool done = false, abort = false;
    while (!done) {
        if (sel < top) top = sel;
        if (sel >= top + show) top = sel - show + 1;
        for (int i = 0; i < show; i++) {
            int k = top + i;
            std::wstring t = k < n ? L"  " + to_cells(items[(size_t)k]) : std::wstring();
            if ((int)t.size() > w) t = t.substr(0, (size_t)w - 3) + L"...";
            put((SHORT)(y0 + i), t, k == sel ? inv : norm);
        }
        INPUT_RECORD r;
        DWORD got = 0;
        if (!ReadConsoleInputW(g_in, &r, 1, &got) || got == 0) { abort = true; break; }
        if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) continue;
        WORD vk = r.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = r.Event.KeyEvent.uChar.UnicodeChar;
        if (vk == VK_UP) sel = (sel + n - 1) % n;
        else if (vk == VK_DOWN) sel = (sel + 1) % n;
        else if (vk == VK_PRIOR) sel = std::max(0, sel - show);
        else if (vk == VK_NEXT) sel = std::min(n - 1, sel + show);
        else if (vk == VK_HOME) sel = 0;
        else if (vk == VK_END) sel = n - 1;
        else if (vk == VK_RETURN) { result = sel; done = true; }
        else if (vk == VK_ESCAPE) done = true;
        else if (ch == 3) { abort = true; break; }
    }
    for (int i = -1; i < show; i++) put((SHORT)(y0 + i), std::wstring(), norm);
    COORD c = {0, (SHORT)(y0 - 1)};
    SetConsoleCursorPosition(g_out, c);
    SetConsoleCursorInfo(g_out, &ci0);
    SetConsoleMode(g_in, mode0);
    if (abort) throw Abort();
    return result;
}

// The template with the values in v ("section.key" -> value) filled in, comments kept, CRLF lines.
std::string render(const std::map<std::string, std::string>& v) {
    std::string out, section;
    for (auto& line : split_lines(kTemplate)) {
        std::string t = trim(line), nl = line;
        size_t eq = line.find('=');
        if (!t.empty() && t[0] == '[') {
            section = lower(trim(t.substr(1, t.find(']') - 1)));
        } else if (!t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos) {
            auto it = v.find(section + "." + lower(trim(line.substr(0, eq))));
            if (it != v.end()) {
                size_t c = std::string::npos;  // the comment: first ';' or '#' after a blank
                for (size_t k = eq + 2; k < line.size() && c == std::string::npos; k++)
                    if ((line[k] == ';' || line[k] == '#') && (line[k - 1] == ' ' || line[k - 1] == '\t')) c = k;
                nl = line.substr(0, eq + 1) + " " + it->second;
                if (c != std::string::npos) {
                    if (nl.size() + 1 < c) nl.resize(c, ' ');
                    else nl += ' ';
                    nl += line.substr(c);
                }
            }
        }
        out += nl + "\r\n";
    }
    while (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) out.resize(out.size() - 2);
    return out;
}

}  // namespace

bool run_setup(const Config* cur, const std::string& path, std::string* err) {
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    if (!GetConsoleMode(g_in, &in_mode) || !GetConsoleMode(g_out, &out_mode)) {
        if (err) *err = "setup needs a console window";
        return false;
    }
    SetConsoleMode(g_in, in_mode | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

    // Keys that setup doesn't ask about keep their current values.
    std::map<std::string, std::string> v;
    if (cur) v = cur->raw;
    auto val = [&](const char* k) { auto it = v.find(k); return it == v.end() ? std::string() : it->second; };

    bool ok = true;
    try {
        say("agent setup\nEnter keeps the value in [brackets], - clears it.\n\n");

        // 1. endpoint
        std::string url = val("llm.base_url");
        if (url.empty()) url = "http://localhost:8080/v1";
        while (true) {
            std::string s = ask("LLM endpoint URL (OpenAI-compatible)", url);
            if (s.empty()) { say("  ! required\n"); continue; }
            s = http_url(s);
            if (s.empty()) { say("  ! only plain http:// is supported\n"); continue; }
            url = s;
            break;
        }

        // 2. API key
        std::string key = ask("API key (optional)", val("llm.api_key"), masked(val("llm.api_key")));

        // 3. model: from the server's list if it has one, else typed
        std::string model = val("llm.model"), lerr;
        std::vector<std::string> models = list_models(url, key, lerr);
        int chosen = -1;
        if (!models.empty()) {
            int sel = 0;
            for (size_t i = 0; i < models.size(); i++)
                if (models[i] == model) sel = (int)i;
            chosen = pick("Model (Up/Down, Enter; Esc to type a name)", models, sel);
            if (chosen >= 0) {
                model = models[(size_t)chosen];
                say("Model: " + model + "\n");
            }
        } else {
            say("  ! can't list the models: " + lerr + "\n");
        }
        while (chosen < 0) {
            model = ask("Model", model);
            if (!model.empty()) break;
            say("  ! required\n");
        }

        // 4. context length
        std::string ctx = val("llm.context_length");
        if (ctx.empty()) ctx = "32768";
        while (true) {
            std::string s = ask("Context length (tokens)", ctx);
            char* end = nullptr;
            long n = strtol(s.c_str(), &end, 10);
            if (!s.empty() && end && *end == 0 && n > 0) { ctx = std::to_string(n); break; }
            say("  ! enter a number\n");
        }

        // 5. Firecrawl
        std::string fc = val("firecrawl.url");
        while (true) {
            std::string s = ask("Firecrawl URL (optional; empty = no web tools)", fc);
            if (!s.empty()) {
                s = http_url(s);
                if (s.empty()) { say("  ! only plain http:// is supported\n"); continue; }
                HttpResult r = get(s + "/", "");
                if (r.status > 0) say("  reachable\n");
                else say("  ! not reachable: " + (r.error.empty() ? std::string("no response") : r.error) + "\n");
            }
            fc = s;
            break;
        }

        // 6. bash
        bool off = lower(trim(val("tools.bash"))) == "none";
        std::string bash = off ? std::string() : cur && !cur->bash_path.empty() ? cur->bash_path : detect_bash();
        while (true) {
            std::string s = ask(bash.empty() && !off ? "bash.exe path (none found; empty = no bash)" : "bash.exe path (empty = no bash)", bash);
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
            if (s.empty() || file_exists(s)) { bash = s.empty() ? s : full_path(s); break; }
            say("  ! not found\n");
        }

        v["llm.base_url"] = url;
        v["llm.api_key"] = key;
        v["llm.model"] = model;
        v["llm.context_length"] = ctx;
        v["firecrawl.url"] = fc;
        v["tools.bash"] = bash.empty() ? "none" : bash;
        // Keep the data dir in use; a new config gets %APPDATA%\agent\data.
        if (cur && !val("agent.data_dir").empty()) {
            v["agent.data_dir"] = cur->data_dir;
        } else {
            std::string ud = user_config_dir();
            v["agent.data_dir"] = path_join(ud.empty() ? path_dir(full_path(path)) : ud, "data");
        }
    } catch (const Abort&) {
        ok = false;
        if (err) *err = "setup cancelled; nothing was saved";
    }
    SetConsoleMode(g_in, in_mode);
    if (!ok) return false;

    make_dirs(path_dir(full_path(path)));
    if (!write_file(path, render(v))) {
        if (err) *err = "can't write " + path + ": " + win_error(GetLastError());
        return false;
    }
    return true;
}

}  // namespace ag
