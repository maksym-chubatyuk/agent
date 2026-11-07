#include "tools.h"
#include "http.h"
#include "proc.h"
#include "util.h"
#include <cstdio>

namespace ag {

// ---------------------------------------------------------------- schemas

static Json fn(const char* name, const char* desc, Json props, std::vector<const char*> required) {
    Json params = Json::object();
    params.set("type", "object").set("properties", props);
    Json req = Json::array();
    for (auto r : required) req.push_back(r);
    params.set("required", req);
    Json f = Json::object();
    f.set("name", name).set("description", desc).set("parameters", params);
    Json t = Json::object();
    t.set("type", "function").set("function", f);
    return t;
}
static Json prop(const char* type, const char* desc) {
    Json p = Json::object();
    p.set("type", type).set("description", desc);
    return p;
}

Json tool_schemas() {
    Json tools = Json::array();
    {
        Json p = Json::object();
        p.set("query", prop("string", "The search query to look up on the web. You may include operators such as "
                                      "site:example.com, filetype:pdf, intitle:word, -term, or \"exact phrase\"."));
        p.set("limit", prop("integer", "Maximum number of results to return (1-20). Defaults to 5."));
        tools.push_back(fn("web_search",
                           "Search the web: returns titles, URLs and snippets (5 by default). Read pages with "
                           "web_scrape. The query is passed through to the search backend, so operators such as "
                           "site:domain, filetype:pdf, intitle:word, -term, and \"exact phrase\" work.",
                           p, {"query"}));
    }
    {
        Json p = Json::object();
        p.set("url", prop("string", "The http(s) URL of the page to read."));
        tools.push_back(fn("web_scrape",
                           "Extract the content of a web page as markdown (no summarization). Also works with PDF "
                           "URLs. Pages within the character budget return whole; larger pages return a head+tail "
                           "window with a footer giving the full text's saved file path, so you can page through "
                           "the omitted middle with read_file or grep it via bash. Images appear as [IMAGE: alt] "
                           "placeholders; links are kept.",
                           p, {"url"}));
    }
    {
        Json p = Json::object();
        p.set("command", prop("string", "The bash command(s) to run."));
        tools.push_back(fn("bash",
                           "Run a command in Git Bash (bash 4.3) on Windows XP. Returns the exit code and combined "
                           "stdout/stderr. No timeout: avoid commands that wait for input or run forever. stdin is "
                           "empty. The working directory persists between calls. Long output is saved to a file and "
                           "you get its head, tail and path. Use this for listing directories, grep, find, git, and "
                           "editing your memory file.",
                           p, {"command"}));
    }
    {
        Json p = Json::object();
        p.set("path", prop("string", "File path (absolute, or relative to the current working directory)."));
        p.set("offset", prop("integer", "First line to return, 1-based (default 1)."));
        p.set("limit", prop("integer", "Maximum number of lines to return (default 500)."));
        tools.push_back(fn("read_file",
                           "Read a text file by lines. Each output line is prefixed with its line number and a tab "
                           "(the prefix is not part of the file).",
                           p, {"path"}));
    }
    {
        Json p = Json::object();
        p.set("path", prop("string", "File path (absolute, or relative to the current working directory)."));
        p.set("content", prop("string", "The complete new file content."));
        tools.push_back(fn("write_file", "Create or overwrite a file with the given content. Parent directories are created.", p,
                           {"path", "content"}));
    }
    {
        Json p = Json::object();
        p.set("path", prop("string", "File path (absolute, or relative to the current working directory)."));
        p.set("old_text", prop("string", "Exact text to replace; must occur exactly once in the file."));
        p.set("new_text", prop("string", "Replacement text."));
        tools.push_back(fn("edit_file",
                           "Replace one exact occurrence of old_text with new_text in a file. Fails if old_text is "
                           "not found or occurs more than once (add surrounding context to make it unique).",
                           p, {"path", "old_text", "new_text"}));
    }
    return tools;
}

// ---------------------------------------------------------------- approval patterns

std::vector<std::string> default_risky_patterns() {
    return {
        "\\brm\\s+(-[a-z]*\\s+)*-[a-z]*[rf]",
        "\\brm\\s.*--(recursive|force)",
        "\\b(rmdir|rd)\\s.*/s\\b",
        "\\b(del|erase)\\s.*/[sq]\\b",
        "\\bformat(\\.com)?\\s+[a-z]:",
        "\\bmkfs",
        "\\bdd\\s.*\\bof=",
        "\\b(shutdown|reboot|poweroff)\\b",
        "\\breg(\\.exe)?\\s+(delete|add|import|restore|load|unload|copy)\\b",
        "\\bregedit(\\.exe)?\\s.*/s\\b",
        "\\b(diskpart|bcdedit|fdisk|bootcfg|fixmbr|fixboot|vssadmin|cipher)\\b",
        "\\b(takeown|cacls|icacls|xcacls)\\b",
        "\\bnet\\s+(user|localgroup|stop|share|accounts)\\b",
        "\\bsc(\\.exe)?\\s+(delete|stop|config|create)\\b",
        "\\b(taskkill|tskill|pkill|killall)\\b",
        "\\bkill\\s+-9?\\s*-1\\b",
        "\\bch(mod|own)\\s+(-[a-z]*\\s+)*-[a-z]*r",
        ":\\(\\)\\s*\\{",
        "\\bgit\\s+(clean\\s+-[a-z]*f|reset\\s+--hard|push\\s.*(--force|-f\\b))",
        ">\\s*/dev/(sd|hd)",
        // writes into system directories (redirection or a file-changing command naming them)
        "(>|\\b(cp|mv|tee|copy|move|xcopy|robocopy|rm|del|erase|rmdir|rd|ren|rename|ln|install|attrib|unzip|tar|sed\\s+-i)\\b).*"
        "(c:|/c)?[\\\\/]+(windows|winnt|program files|boot\\.ini|ntldr|pagefile\\.sys|documents and settings[\\\\/]+all users)",
        "\\b(system32|syswow64)\\b.*(>|\\b(cp|mv|copy|move|del|rm)\\b)",
        "(>|\\b(cp|mv|copy|move|del|rm)\\b).*\\b(system32|syswow64)\\b",
    };
}

std::vector<Regex> load_risky_patterns(const std::string& extra_file, std::vector<std::string>* errors) {
    std::vector<std::string> pats = default_risky_patterns();
    std::string text;
    if (!extra_file.empty() && read_file(extra_file, text)) {
        for (auto& l : split_lines(text)) {
            std::string t = trim(l);
            if (t.empty() || t[0] == '#') continue;
            pats.push_back(t);
        }
    }
    std::vector<Regex> out;
    for (auto& p : pats) {
        Regex r;
        std::string err;
        if (r.compile(p, &err)) out.push_back(r);
        else if (errors) errors->push_back("bad approval pattern '" + p + "': " + err);
    }
    return out;
}

std::string check_risky(const std::string& command, const std::vector<Regex>& pats) {
    for (auto& r : pats)
        if (r.search(command)) return r.pattern();
    return "";
}

std::string clean_search_query(const std::string& q) {
    // Operators and quoted phrases are passed through to the search backend; only whitespace is tidied.
    std::string out;
    bool sp = false;
    for (char c : q) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = true; continue; }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += c;
    }
    return out;
}

// Markdown cleanup for scraped pages: images become [IMAGE: alt] placeholders (dropped when they have no
// alt text), links whose text was only an image are dropped, trailing spaces go and runs of blank lines
// collapse to one.
static size_t md_close(const std::string& s, size_t open, char oc, char cc) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        if (s[i] == oc) depth++;
        else if (s[i] == cc && --depth == 0) return i;
        else if (s[i] == '\n' && oc == '[' && i + 1 < s.size() && s[i + 1] == '\n') return std::string::npos;
    }
    return std::string::npos;
}

static std::string md_strip_images(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '!' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t rb = md_close(s, i + 1, '[', ']');
            if (rb != std::string::npos && rb + 1 < s.size() && s[rb + 1] == '(') {
                size_t rp = md_close(s, rb + 1, '(', ')');
                if (rp != std::string::npos) {
                    std::string alt = trim(replace_all(s.substr(i + 2, rb - i - 2), "\n", " "));
                    if (!alt.empty()) out += "[IMAGE: " + alt + "]";
                    i = rp + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

static std::string md_drop_empty_links(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '[' && (i == 0 || s[i - 1] != '!')) {
            size_t rb = md_close(s, i, '[', ']');
            if (rb != std::string::npos && rb + 1 < s.size() && s[rb + 1] == '(' && trim(s.substr(i + 1, rb - i - 1)).empty()) {
                size_t rp = md_close(s, rb + 1, '(', ')');
                if (rp != std::string::npos) { i = rp + 1; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

static std::string clean_scraped_markdown(const std::string& md) {
    std::string s = md_drop_empty_links(md_strip_images(replace_all(md, "\r\n", "\n")));
    std::string out;
    int blank = 0;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (trim(line).empty()) {
            if (++blank == 1 && !out.empty()) out += "\n";
        } else {
            blank = 0;
            out += line + "\n";
        }
        pos = nl + 1;
    }
    return trim(out) + "\n";
}

// ---------------------------------------------------------------- helpers

static ToolResult fail(const std::string& msg) {
    ToolResult r;
    r.ok = false;
    r.content = "Error: " + msg;
    r.summary = utf8_head(msg, 200);
    return r;
}

static std::string resolve_path(const std::string& p0, const ToolContext& ctx) {
    std::string p = msys_to_win(trim(p0));
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/' || p[1] == '\\')) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        if (n && n < MAX_PATH) p = narrow(std::wstring(buf, n)) + p.substr(1);
    }
    if (!path_is_absolute(p)) p = path_join(ctx.cwd ? *ctx.cwd : ctx.cfg->workdir, p);
    return full_path(to_backslashes(p));
}

static unsigned g_spill_counter = 0;
// Files of this conversation live in its scratchpad (fallback: <data_dir>\tmp).
static std::string scratch_dir(const ToolContext& ctx) {
    std::string d = ctx.scratch.empty() ? path_join(ctx.cfg->data_dir, "tmp") : ctx.scratch;
    make_dirs(d);
    return d;
}
static std::string new_spill_path(const ToolContext& ctx, const char* prefix, const char* ext) {
    return path_join(scratch_dir(ctx), std::string(prefix) + "-" + now_stamp() + "-" + std::to_string(++g_spill_counter) + ext);
}

static void http_json(const Config& cfg, ToolContext& ctx, const std::string& url, const Json& body, int timeout_ms,
                      HttpResult& res, Json& out, std::string& err) {
    HttpOptions opt;
    opt.connect_timeout_ms = cfg.connect_timeout_s * 1000;
    opt.idle_timeout_ms = timeout_ms;
    opt.cancel_event = ctx.cancel_event;
    http_request("POST", url, {{"Content-Type", "application/json"}, {"Accept", "application/json"}}, body.dump(false), opt,
                 nullptr, res);
    if (res.cancelled) { err = "cancelled"; return; }
    if (!res.error.empty() && res.status == 0) { err = res.error; return; }
    std::string perr;
    if (!Json::parse(res.body, out, &perr)) {
        err = "HTTP " + std::to_string(res.status) + ", unparsable response (" + perr + "): " + utf8_head(res.body, 300);
        return;
    }
    if (res.status < 200 || res.status >= 300 || out.get_bool("success", true) == false) {
        std::string e = out.get_str("error");
        if (e.empty()) e = utf8_head(res.body, 300);
        err = "HTTP " + std::to_string(res.status) + ": " + e;
    }
}

// ---------------------------------------------------------------- web_search

static ToolResult tool_web_search(const Json& args, ToolContext& ctx) {
    const Config& cfg = *ctx.cfg;
    if (cfg.firecrawl_url.empty()) return fail("web_search is unavailable: [firecrawl] url is not set in the config");
    std::string q = clean_search_query(args.get_str("query"));
    if (q.empty()) return fail("query is empty");
    long long limit = args["limit"].is_number() ? args["limit"].as_int() : cfg.search_limit;
    if (args["limit"].is_string()) limit = atoi(args["limit"].as_string().c_str());
    if (limit < 1) limit = 1;
    if (limit > 20) limit = 20;
    Json body = Json::object();
    body.set("query", q).set("limit", limit);
    HttpResult res;
    Json out;
    std::string err;
    http_json(cfg, ctx, cfg.firecrawl_url + "/v2/search", body, 90000, res, out, err);
    if (!err.empty()) {
        ToolResult r = fail("web_search failed: " + err);
        r.cancelled = res.cancelled;
        return r;
    }
    const Json& data = out["data"];
    std::vector<const Json*> items;
    if (data.is_array()) for (auto& it : data.items()) items.push_back(&it);
    else if (data.is_object()) {
        for (const char* k : {"web", "news"})
            for (auto& it : data[k].items()) items.push_back(&it);
    }
    std::string text = "Search results for: " + q + "\n";
    int n = 0;
    for (auto* it : items) {
        n++;
        std::string title = it->get_str("title");
        std::string url = it->get_str("url");
        std::string snip = it->get_str("description");
        if (snip.empty()) snip = it->get_str("snippet");
        if (snip.empty()) snip = utf8_head(it->get_str("markdown"), 300);
        text += "\n" + std::to_string(n) + ". " + (title.empty() ? "(no title)" : title) + "\n   " + url + "\n";
        if (!snip.empty()) text += "   " + replace_all(trim(snip), "\n", " ") + "\n";
        if (n >= limit) break;
    }
    if (n == 0) text += "\n(no results)\n";
    ToolResult r;
    r.content = text;
    r.summary = std::to_string(n) + " result(s) for \"" + q + "\"";
    return r;
}

// ---------------------------------------------------------------- web_scrape

static ToolResult tool_web_scrape(const Json& args, ToolContext& ctx) {
    const Config& cfg = *ctx.cfg;
    if (cfg.firecrawl_url.empty()) return fail("web_scrape is unavailable: [firecrawl] url is not set in the config");
    std::string url = trim(args.get_str("url"));
    if (url.empty()) return fail("url is empty");
    if (!starts_with(lower(url), "http://") && !starts_with(lower(url), "https://")) url = "https://" + url;
    Json body = Json::object();
    Json formats = Json::array();
    formats.push_back("markdown");
    // Firecrawl's onlyMainContent mode deletes elements whose class names look like page chrome (".widget",
    // ".top", ".side", ...), which on many sites includes the article itself. Ask for the whole page and
    // remove only the standard page-chrome elements instead.
    Json exclude = Json::array();
    for (const char* t : {"nav", "header", "footer", "aside", "[role=navigation]", "[role=banner]",
                          "[role=contentinfo]", "[role=complementary]"})
        exclude.push_back(t);
    body.set("url", url).set("formats", formats).set("onlyMainContent", false).set("excludeTags", exclude)
        .set("timeout", cfg.scrape_timeout_ms);
    HttpResult res;
    Json out;
    std::string err;
    http_json(cfg, ctx, cfg.firecrawl_url + "/v2/scrape", body, cfg.scrape_timeout_ms + 30000, res, out, err);
    if (!err.empty()) {
        ToolResult r = fail("web_scrape failed: " + err);
        r.cancelled = res.cancelled;
        return r;
    }
    const Json& data = out["data"];
    std::string raw = data.get_str("markdown");
    std::string md = clean_scraped_markdown(raw);
    std::string title = data["metadata"].get_str("title");
    long long code = data["metadata"].get_int("statusCode", 0);
    std::string file = new_spill_path(ctx, "scrape", ".md");
    std::string header = "<!-- source: " + url + (title.empty() ? "" : " | title: " + title) + " -->\n";
    bool saved = write_file(file, header + raw);
    ToolResult r;
    r.output_file = saved ? file : "";
    std::string info = "URL: " + url + "\n" + (title.empty() ? "" : "Title: " + title + "\n") +
                       (code ? "HTTP status: " + std::to_string(code) + "\n" : "") + "Length: " + std::to_string(md.size()) +
                       " bytes, " + std::to_string(count_occurrences(md, "\n") + 1) + " lines\n" +
                       (saved ? "Full page (raw markdown) saved to: " + file + "\n" : "(could not save the page to a file)\n");
    if ((long)md.size() <= cfg.scrape_chars) {
        r.content = info + "\n" + md;
    } else {
        size_t tail_n = (size_t)cfg.scrape_chars / 5, head_n = (size_t)cfg.scrape_chars - tail_n;
        r.content = info + "\n" + utf8_head(md, head_n) + "\n\n[... " + std::to_string(md.size() - head_n - tail_n) +
                    " characters omitted. The full page is in the file above: read it with read_file (offset/limit) "
                    "or search it with grep via bash ...]\n\n" + utf8_tail(md, tail_n);
    }
    r.summary = (title.empty() ? url : title) + " (" + std::to_string(md.size()) + " bytes)";
    return r;
}

// ---------------------------------------------------------------- bash

namespace {
struct OutputCollector {
    long threshold, tail_keep;
    std::string head;          // first bytes (up to threshold)
    std::string tail;          // rolling tail once spilling
    std::string file;
    HANDLE fh = INVALID_HANDLE_VALUE;
    bool spilling = false;
    bool oem = false;          // convert to UTF-8 from OEM when writing the file
    long long total = 0, lines = 0;
    const Config* cfg;
    const ToolContext* ctx;

    void write_out(const char* p, size_t n) {
        if (fh == INVALID_HANDLE_VALUE) return;
        std::string s(p, n);
        if (oem) s = codepage_to_utf8(s, CP_OEMCP);
        DWORD w;
        WriteFile(fh, s.data(), (DWORD)s.size(), &w, nullptr);
    }
    void add(const char* p, size_t n) {
        total += n;
        for (size_t i = 0; i < n; i++) if (p[i] == '\n') lines++;
        if (!spilling) {
            head.append(p, n);
            if ((long)head.size() > threshold) {
                spilling = true;
                file = new_spill_path(*ctx, "bash", ".txt");
                oem = !valid_utf8(utf8_head(head, head.size() - 4));
                fh = CreateFileW(widen(file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
                write_out(head.data(), head.size());
                tail = head.size() > (size_t)tail_keep ? head.substr(head.size() - tail_keep) : head;
            }
            return;
        }
        write_out(p, n);
        tail.append(p, n);
        if (tail.size() > (size_t)tail_keep * 4) tail.erase(0, tail.size() - tail_keep);
    }
    void close() {
        if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        fh = INVALID_HANDLE_VALUE;
    }
};

std::string sh_quote(const std::string& s) { return "'" + replace_all(s, "'", "'\\''") + "'"; }

std::string git_root(const std::string& bash) {
    std::string d = path_dir(bash);  // ...\bin or ...\usr\bin
    std::string l = lower(to_backslashes(d));
    if (ends_with(l, "\\usr\\bin")) return path_dir(path_dir(d));
    if (ends_with(l, "\\bin")) return path_dir(d);
    return d;
}
}  // namespace

static unsigned g_cmd_counter = 0;

static ToolResult tool_bash(const Json& args, ToolContext& ctx) {
    const Config& cfg = *ctx.cfg;
    std::string command = args.get_str("command");
    if (trim(command).empty()) return fail("command is empty");
    if (!ctx.yolo && ctx.risky) {
        std::string why = check_risky(command, *ctx.risky);
        if (!why.empty()) {
            bool ok = ctx.approve ? ctx.approve(command, why) : false;
            if (!ok) {
                ToolResult r;
                r.ok = false;
                r.denied = true;
                r.cancelled = ctx.cancel_event && WaitForSingleObject(ctx.cancel_event, 0) == WAIT_OBJECT_0;
                r.content = r.cancelled ? "The command was not run: the turn was cancelled while waiting for approval."
                                        : "The user denied permission to run this command (it matched the risky-command "
                                          "pattern " + why + "). Do not retry it; ask the user or choose a safer approach.";
                r.summary = "denied";
                return r;
            }
        }
    }
    if (cfg.bash_off) return fail("bash is turned off in the config ([tools] bash = none)");
    if (cfg.bash_path.empty()) return fail("no bash.exe configured ([tools] bash) and none found on PATH or in the usual Git for Windows folders");
    if (!file_exists(cfg.bash_path)) return fail("bash not found at " + cfg.bash_path + " (check [tools] bash in the config)");

    std::string cwd = ctx.cwd && !ctx.cwd->empty() ? *ctx.cwd : cfg.workdir;
    if (!dir_exists(cwd)) {
        if (to_backslashes(lower(cwd)) == lower(cfg.workdir)) make_dirs(cwd);
        if (!dir_exists(cwd)) cwd = cfg.workdir, make_dirs(cwd);
    }
    std::string tmpdir = scratch_dir(ctx);
    unsigned n = ++g_cmd_counter;
    std::string tag = std::to_string(n);
    std::string script = path_join(tmpdir, "cmd-" + tag + ".sh");
    std::string cwdfile = path_join(tmpdir, "cwd-" + tag + ".txt");
    std::string outfile = path_join(tmpdir, "out-" + tag + ".log");
    if (ctx.background) bg_prune(*ctx.background);  // forget background jobs that have finished
    std::string body = "cd " + sh_quote(to_slashes(cwd)) + " 2>/dev/null\n" +
                       "__agent_cwdfile=" + sh_quote(to_slashes(cwdfile)) + "\n" +
                       "trap 'pwd -W > \"$__agent_cwdfile\" 2>/dev/null' EXIT\n" + command + "\n";
    if (!write_file(script, body)) return fail("cannot write " + script);

    std::string root = git_root(cfg.bash_path);
    std::string sysroot;
    {
        wchar_t buf[MAX_PATH];
        UINT k = GetWindowsDirectoryW(buf, MAX_PATH);
        if (k && k < MAX_PATH) sysroot = narrow(std::wstring(buf, k));
    }
    std::string path = (cfg.path_extra.empty() ? "" : cfg.path_extra + ";") + root + "\\usr\\bin;" + root + "\\mingw32\\bin;" +
                       root + "\\bin;" + sysroot + "\\system32;" + sysroot + ";" + sysroot + "\\system32\\Wbem";
    std::vector<std::pair<std::string, std::string>> env = {
        {"PATH", path},
        {"MSYSTEM", "MINGW32"},
        {"TERM", "dumb"},
        {"PAGER", "cat"},
        {"GIT_PAGER", "cat"},
        {"GIT_TERMINAL_PROMPT", "0"},
        {"AGENT_DATA", to_slashes(cfg.data_dir)},
        {"AGENT_MEMORY", to_slashes(cfg.memory_path())},
        {"AGENT_SCRATCH", to_slashes(tmpdir)},
    };

    ProcRequest pr;
    pr.app = cfg.bash_path;
    pr.cmdline = quote_arg(cfg.bash_path) + " --noprofile --norc " + quote_arg(to_slashes(script));
    pr.cwd = cwd;
    pr.env_block = build_env_block(env);
    pr.cancel_event = ctx.cancel_event;
    pr.output_file = outfile;
    pr.background = ctx.background;
    OutputCollector col;
    col.threshold = cfg.spill_threshold;
    col.tail_keep = cfg.spill_tail;
    col.cfg = &cfg;
    col.ctx = &ctx;
    pr.on_output = [&](const char* p, size_t k) { col.add(p, k); };
    ProcResult res;
    unsigned long t0 = tick_ms();
    proc_run(pr, res);
    col.close();
    unsigned long dt = tick_ms() - t0;

    std::string newcwd;
    if (read_file(cwdfile, newcwd)) {
        newcwd = trim(newcwd);
        if (!newcwd.empty() && ctx.cwd) *ctx.cwd = to_backslashes(newcwd);
    }
    delete_file(script);
    delete_file(cwdfile);
    if (res.background_left == 0) delete_file(outfile);  // else background processes still write to it

    if (!res.started) return fail("could not start bash: " + res.error);
    if (!res.tree_kill_by_job) log_error("bash: " + res.error);

    ToolResult r;
    r.has_exit_code = true;
    r.exit_code = res.exit_code;
    r.cancelled = res.cancelled;
    r.ok = !res.cancelled;
    std::string text;
    if (res.cancelled) text = "Command cancelled by the user after " + std::to_string(dt / 1000) + " s; its process tree was killed.\n";
    text += "exit code: " + std::to_string(res.exit_code) + "\n";
    if (!col.spilling) {
        text += output_to_utf8(col.head);
        if (col.head.empty()) text += "(no output)\n";
        r.summary = "exit " + std::to_string(res.exit_code) + ", " + std::to_string(col.lines) + " line(s)";
    } else {
        r.output_file = col.file;
        text += "Output was " + std::to_string(col.total) + " bytes / " + std::to_string(col.lines) +
                " lines; saved in full to: " + col.file + "\n--- first " + std::to_string(cfg.spill_head) + " bytes ---\n" +
                output_to_utf8(utf8_head(col.head, cfg.spill_head)) + "\n";
        if (cfg.spill_tail > 0)
            text += "--- last " + std::to_string(cfg.spill_tail) + " bytes ---\n" + output_to_utf8(utf8_tail(col.tail, cfg.spill_tail)) + "\n";
        text += "--- (use read_file with offset/limit, or grep, on the file above for the rest) ---\n";
        r.summary = "exit " + std::to_string(res.exit_code) + ", " + std::to_string(col.lines) + " lines, spilled to file";
    }
    if (res.background_left > 0) {
        text += "[" + std::to_string(res.background_left) + " background process(es) from this command are still running. "
                "They keep running until the conversation ends, then they are killed. Their further output goes to: " +
                outfile + "]\n";
        r.summary += ", " + std::to_string(res.background_left) + " background process(es) left running";
    }
    if (res.cancelled) r.summary = "cancelled (tree killed), " + r.summary;
    r.content = text;
    return r;
}

// ---------------------------------------------------------------- read_file

static ToolResult tool_read_file(const Json& args, ToolContext& ctx) {
    const Config& cfg = *ctx.cfg;
    std::string p = args.get_str("path");
    if (trim(p).empty()) return fail("path is empty");
    std::string path = resolve_path(p, ctx);
    if (dir_exists(path)) return fail(path + " is a directory (use bash ls)");
    long long offset = args["offset"].is_number() ? args["offset"].as_int() : 1;
    long long limit = args["limit"].is_number() ? args["limit"].as_int() : cfg.read_limit;
    if (args["offset"].is_string()) offset = atoll(args["offset"].as_string().c_str());
    if (args["limit"].is_string()) limit = atoll(args["limit"].as_string().c_str());
    if (offset < 1) offset = 1;
    if (limit < 1) limit = cfg.read_limit;
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("cannot open " + path + ": " + win_error(GetLastError()));
    std::string out, line;
    long long lineno = 0, shown_from = 0, shown_to = 0;
    bool stopped_by_bytes = false, first_chunk = true, binary = false;
    long long total_bytes = 0;
    std::vector<char> bufv(65536);
    char* buf = bufv.data();
    char lastc = '\n';
    DWORD got;
    auto flush_line = [&](bool final_partial) {
        (void)final_partial;
        lineno++;
        if (lineno >= offset && lineno < offset + limit && !stopped_by_bytes) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string l = valid_utf8(line) ? line : codepage_to_utf8(line, CP_ACP);
            if (l.size() > 4000) l = utf8_head(l, 4000) + " [line truncated, " + std::to_string(line.size()) + " bytes]";
            char num[24];
            snprintf(num, sizeof num, "%6lld\t", lineno);
            if ((long)(out.size() + l.size()) > cfg.read_max_bytes && shown_from) { stopped_by_bytes = true; }
            else {
                out += num;
                out += l;
                out += '\n';
                if (!shown_from) shown_from = lineno;
                shown_to = lineno;
            }
        }
        line.clear();
    };
    while (ReadFile(h, buf, (DWORD)bufv.size(), &got, nullptr) && got > 0) {
        if (first_chunk) {
            first_chunk = false;
            for (DWORD i = 0; i < got && i < 8192; i++) if (buf[i] == 0) { binary = true; break; }
            if (binary) break;
        }
        total_bytes += got;
        lastc = buf[got - 1];
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n') flush_line(false);
            else if (lineno + 1 >= offset && lineno + 1 < offset + limit && !stopped_by_bytes) line += buf[i];
        }
    }
    if (!binary && total_bytes > 0 && lastc != '\n') flush_line(true);
    CloseHandle(h);
    if (binary) {
        long long sz = file_size(path);
        return fail(path + " looks like a binary file (" + std::to_string(sz) + " bytes); use bash (od, xxd, strings) to inspect it");
    }
    ToolResult r;
    std::string head = "File: " + path + " (" + std::to_string(lineno) + " lines, " + std::to_string(total_bytes) + " bytes)\n";
    if (lineno == 0) { r.content = head + "(empty file)\n"; r.summary = "empty file"; return r; }
    if (!shown_from) {
        r.content = head + "(offset " + std::to_string(offset) + " is past the end of the file)\n";
        r.summary = "offset past end";
        return r;
    }
    r.content = head + out;
    if (shown_to < lineno)
        r.content += "[showing lines " + std::to_string(shown_from) + "-" + std::to_string(shown_to) + " of " +
                     std::to_string(lineno) + "; use offset=" + std::to_string(shown_to + 1) + " to continue]\n";
    r.summary = "lines " + std::to_string(shown_from) + "-" + std::to_string(shown_to) + " of " + std::to_string(lineno);
    return r;
}

// ---------------------------------------------------------------- write_file / edit_file

static ToolResult tool_write_file(const Json& args, ToolContext& ctx) {
    std::string p = args.get_str("path");
    if (trim(p).empty()) return fail("path is empty");
    if (!args["content"].is_string()) return fail("content must be a string");
    const std::string& content = args["content"].as_string();
    std::string path = resolve_path(p, ctx);
    if (dir_exists(path)) return fail(path + " is a directory");
    bool existed = file_exists(path);
    make_dirs(path_dir(path));
    if (!write_file(path, content)) return fail("cannot write " + path + ": " + win_error(GetLastError()));
    ToolResult r;
    long long lines = content.empty() ? 0 : (long long)count_occurrences(content, "\n") + (content.back() == '\n' ? 0 : 1);
    r.content = std::string(existed ? "Overwrote " : "Created ") + path + " (" + std::to_string(content.size()) + " bytes, " +
                std::to_string(lines) + " lines)";
    r.summary = std::string(existed ? "overwrote " : "created ") + path;
    return r;
}

static ToolResult tool_edit_file(const Json& args, ToolContext& ctx) {
    std::string p = args.get_str("path");
    if (trim(p).empty()) return fail("path is empty");
    if (!args["old_text"].is_string() || !args["new_text"].is_string()) return fail("old_text and new_text must be strings");
    std::string old_text = args["old_text"].as_string(), new_text = args["new_text"].as_string();
    if (old_text.empty()) return fail("old_text is empty");
    std::string path = resolve_path(p, ctx);
    std::string data;
    if (!file_exists(path)) return fail(path + " does not exist");
    if (!read_file(path, data)) return fail("cannot read " + path + ": " + win_error(GetLastError()));
    size_t n = count_occurrences(data, old_text);
    if (n == 0 && data.find("\r\n") != std::string::npos) {  // file uses CRLF, model sent LF
        std::string o2 = replace_all(replace_all(old_text, "\r\n", "\n"), "\n", "\r\n");
        size_t n2 = count_occurrences(data, o2);
        if (n2 >= 1) {
            old_text = o2;
            new_text = replace_all(replace_all(new_text, "\r\n", "\n"), "\n", "\r\n");
            n = n2;
        }
    }
    if (n == 0) return fail("old_text was not found in " + path + " (it must match exactly, including whitespace)");
    if (n > 1) return fail("old_text occurs " + std::to_string(n) + " times in " + path + "; include more surrounding text so it matches exactly once");
    size_t pos = data.find(old_text);
    data.replace(pos, old_text.size(), new_text);
    if (!write_file(path, data)) return fail("cannot write " + path + ": " + win_error(GetLastError()));
    long long line = 1;
    for (size_t i = 0; i < pos; i++) if (data[i] == '\n') line++;
    ToolResult r;
    r.content = "Edited " + path + ": replaced 1 occurrence at line " + std::to_string(line) + " (" + std::to_string(old_text.size()) +
                " -> " + std::to_string(new_text.size()) + " bytes)";
    r.summary = "edited " + path + " at line " + std::to_string(line);
    return r;
}

// ---------------------------------------------------------------- dispatch

ToolResult run_tool(const std::string& name, const Json& args, ToolContext& ctx) {
    if (!args.is_object()) return fail("arguments must be a JSON object");
    if (name == "web_search") return tool_web_search(args, ctx);
    if (name == "web_scrape") return tool_web_scrape(args, ctx);
    if (name == "bash") return tool_bash(args, ctx);
    if (name == "read_file") return tool_read_file(args, ctx);
    if (name == "write_file") return tool_write_file(args, ctx);
    if (name == "edit_file") return tool_edit_file(args, ctx);
    return fail("unknown tool '" + name + "'. Available: web_search, web_scrape, bash, read_file, write_file, edit_file");
}

}  // namespace ag
