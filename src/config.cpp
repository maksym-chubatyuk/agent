#include "config.h"
#include "util.h"
#include <cstdlib>
#include <vector>

namespace ag {

static bool parse_bool(const std::string& v) {
    std::string s = lower(trim(v));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool Config::load(const std::string& path, std::string* err, const std::map<std::string, std::string>* overrides) {
    source_path = path;
    std::string text;
    if (!path.empty() && read_file(path, text)) {
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
            text = text.substr(3);
        std::string section;
        int lineno = 0;
        for (auto& line0 : split_lines(text)) {
            lineno++;
            std::string line = trim(line0);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '[') {
                size_t e = line.find(']');
                if (e == std::string::npos) {
                    if (err) *err = path + ":" + std::to_string(lineno) + ": bad section header";
                    return false;
                }
                section = lower(trim(line.substr(1, e - 1)));
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                if (err) *err = path + ":" + std::to_string(lineno) + ": expected key = value";
                return false;
            }
            std::string key = lower(trim(line.substr(0, eq)));
            std::string val = trim(line.substr(eq + 1));
            // strip inline comment " ;" or " #" (a value that starts with ; or # is empty)
            if (!val.empty() && (val[0] == ';' || val[0] == '#')) val.clear();
            for (size_t k = 1; k < val.size(); k++) {
                if ((val[k] == ';' || val[k] == '#') && (val[k - 1] == ' ' || val[k - 1] == '\t')) {
                    val = trim(val.substr(0, k));
                    break;
                }
            }
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
            raw[(section.empty() ? "" : section + ".") + key] = val;
        }
    }
    if (overrides)
        for (auto& o : *overrides) raw[lower(o.first)] = o.second;
    auto S = [&](const char* k, std::string& dst) { auto it = raw.find(k); if (it != raw.end()) dst = it->second; };
    auto L = [&](const char* k, long& dst) { auto it = raw.find(k); if (it != raw.end() && !it->second.empty()) dst = atol(it->second.c_str()); };
    auto I = [&](const char* k, int& dst) { auto it = raw.find(k); if (it != raw.end() && !it->second.empty()) dst = atoi(it->second.c_str()); };
    auto D = [&](const char* k, double& dst) { auto it = raw.find(k); if (it != raw.end() && !it->second.empty()) dst = atof(it->second.c_str()); };
    auto B = [&](const char* k, bool& dst) { auto it = raw.find(k); if (it != raw.end() && !it->second.empty()) dst = parse_bool(it->second); };

    S("llm.base_url", llm_base_url);
    S("llm.model", llm_model);
    S("llm.api_key", llm_api_key);
    L("llm.context_length", context_length);
    D("llm.compress_threshold", compress_threshold);
    D("llm.keep_recent", keep_recent);
    D("llm.chars_per_token", chars_per_token);
    B("llm.stream_usage", stream_usage);
    L("llm.max_tokens", max_tokens);
    D("llm.temperature", temperature);
    D("llm.top_p", top_p);
    S("llm.reasoning_effort", reasoning_effort);
    I("llm.max_retries", max_retries);
    I("llm.connect_timeout", connect_timeout_s);
    I("llm.idle_timeout", idle_timeout_s);
    S("firecrawl.url", firecrawl_url);
    I("firecrawl.scrape_timeout_ms", scrape_timeout_ms);
    I("firecrawl.search_limit", search_limit);
    L("firecrawl.scrape_chars", scrape_chars);
    S("tools.bash", bash_path);
    S("tools.workdir", workdir);
    S("tools.path_extra", path_extra);
    L("tools.spill_threshold", spill_threshold);
    L("tools.spill_head", spill_head);
    L("tools.spill_tail", spill_tail);
    L("tools.read_limit", read_limit);
    L("tools.read_max_bytes", read_max_bytes);
    S("agent.data_dir", data_dir);
    S("agent.persona_file", persona_file);
    S("agent.memory_file", memory_file);
    S("agent.approval_patterns_file", approval_patterns_file);
    B("agent.yolo", yolo);
    S("agent.machine_note", machine_note);
    finalize();
    return true;
}

void Config::finalize() {
    std::string base = source_path.empty() ? exe_dir() : path_dir(full_path(source_path));
    if (base.empty()) base = exe_dir();
    if (data_dir.empty()) data_dir = path_join(exe_dir(), "data");
    if (!path_is_absolute(data_dir)) data_dir = full_path(path_join(base, data_dir));
    data_dir = to_backslashes(data_dir);
    while (data_dir.size() > 3 && data_dir.back() == '\\') data_dir.pop_back();
    if (workdir.empty()) workdir = path_join(data_dir, "work");
    if (lower(trim(bash_path)) == "none") {
        bash_path.clear();
        bash_off = true;
    } else if (bash_path.empty()) {
        bash_path = detect_bash();
        bash_autodetected = !bash_path.empty();
    } else if (!path_is_absolute(bash_path)) {
        bash_path = full_path(path_join(base, bash_path));
    }
    if (!path_is_absolute(workdir)) workdir = full_path(path_join(base, workdir));
    workdir = to_backslashes(workdir);
    while (!llm_base_url.empty() && llm_base_url.back() == '/') llm_base_url.pop_back();
    while (!firecrawl_url.empty() && firecrawl_url.back() == '/') firecrawl_url.pop_back();
    if (context_length < 256) context_length = 256;
    if (compress_threshold <= 0.05 || compress_threshold > 1.0) compress_threshold = 0.75;
    if (keep_recent <= 0.0 || keep_recent >= compress_threshold) keep_recent = compress_threshold / 2;
    if (chars_per_token < 1) chars_per_token = 3.5;
    if (spill_threshold < 1000) spill_threshold = 1000;
    if (spill_head < 200) spill_head = 200;
    if (spill_head > spill_threshold) spill_head = spill_threshold;
    if (spill_tail < 0) spill_tail = 0;
    if (scrape_chars < 2000) scrape_chars = 2000;
    if (read_limit < 1) read_limit = 500;
    if (max_retries < 0) max_retries = 0;
}

std::string Config::check() const {
    if (llm_base_url.empty()) return "llm.base_url is not set in the config file";
    if (llm_model.empty()) return "llm.model is not set in the config file";
    return "";
}

static std::string env(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? narrow(std::wstring(buf, n)) : std::string();
}

std::string detect_bash() {
    // 1. PATH (skip System32, where newer Windows has the WSL launcher bash.exe)
    std::string path = env(L"PATH");
    size_t start = 0;
    while (start <= path.size()) {
        size_t semi = path.find(';', start);
        std::string dir = trim(path.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        start = semi == std::string::npos ? path.size() + 1 : semi + 1;
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty() || lower(dir).find("system32") != std::string::npos) continue;
        std::string cand = path_join(dir, "bash.exe");
        if (file_exists(cand)) return full_path(cand);
    }
    // 2. common Git for Windows install folders
    std::vector<std::string> roots;
    for (const wchar_t* v : {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
        std::string r = env(v);
        if (!r.empty()) roots.push_back(path_join(r, "Git"));
    }
    std::string lad = env(L"LOCALAPPDATA");
    if (!lad.empty()) roots.push_back(path_join(lad, "Programs\\Git"));
    std::string up = env(L"USERPROFILE");
    if (!up.empty()) roots.push_back(path_join(up, "AppData\\Local\\Programs\\Git"));
    for (auto& r : roots)
        for (const char* sub : {"bin\\bash.exe", "usr\\bin\\bash.exe"}) {
            std::string cand = path_join(r, sub);
            if (file_exists(cand)) return cand;
        }
    // 3. PortableGit unpacked at the root of the system drive
    std::string sd = env(L"SystemDrive");
    if (!sd.empty()) {
        std::string cand = sd + "\\PortableGit\\bin\\bash.exe";
        if (file_exists(cand)) return cand;
    }
    return "";
}

std::string user_config_dir() {
    std::string a = env(L"APPDATA");
    return a.empty() ? std::string() : path_join(a, "agent");
}

std::string Config::persona_path() const { return path_join(data_dir, persona_file); }
std::string Config::memory_path() const { return path_join(data_dir, memory_file); }
std::string Config::patterns_path() const { return path_join(data_dir, approval_patterns_file); }
std::string Config::sessions_dir() const { return path_join(data_dir, "sessions"); }
std::string Config::scratch_root() const { return path_join(data_dir, "scratch"); }

}  // namespace ag
