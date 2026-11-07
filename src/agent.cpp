#include "agent.h"
#include "http.h"
#include "proc.h"
#include "tools.h"
#include "util.h"
#include <process.h>
#include <deque>
#include <cstdio>

namespace ag {

namespace {

// Default identity when there is no persona file. First sentence after opencode's system prompt; the rest is
// Hermes Agent's DEFAULT_AGENT_IDENTITY (agent/prompt_builder.py) without its product name.
const char* kDefaultPersona =
    "You are a helpful assistant. Help the user accomplish their goals using the tools you have available.\n"
    "Be direct: match the length of your reply to the weight of the ask - a one-line question gets a one-line "
    "answer, and finished work gets a short report of what changed, what's verified, and what's left, never a "
    "replay of the process. No filler (\"Great question,\" \"I'd be happy to\"), no restating the request back, no "
    "re-summarizing what you already said, no narrating tool calls the user can see. Plain claims over adjectives; "
    "when unsure, say so plainly. Agree because it's right, not because the user said it. Depth is earned - give "
    "it when the user asks for detail, teaches, or the stakes demand it, not by default.\n";

const char* kDefaultMemory = "# Agent memory\n\n";

// Tool rules for the system prompt, one bullet each, taken from opencode (session/system-prompt.ts, gpt.txt),
// pi (core/system-prompt.ts) and Hermes Agent (agent/prompt_builder.py), with tool names mapped to this
// agent's tools.
const char* kToolRules =
    "# Tools\n"
    "- Prefer parallelizing independent tool calls.\n"
    "- Prefer dedicated tools over shell commands; fall back to the shell when a tool cannot do what you need.\n"
    "- Use write_file to create files or completely replace their content. Prefer edit_file for targeted changes.\n"
    "- Use bash for file operations like ls, grep, find.\n"
    "- Current facts (weather, news, versions, prices) -> search with web_search, then read the most relevant "
    "pages with web_scrape.\n"
    "- If a tool returns empty, partial, or suspiciously narrow results, retry with a broader or different query or "
    "strategy before concluding.\n"
    "- If required context is missing, do NOT guess or hallucinate an answer. Use a tool when the information is "
    "retrievable; ask a clarifying question only when it cannot be retrieved.\n"
    "- Show file paths clearly when working with files.\n"
    "- Never run destructive commands such as recursive deletes on broad paths unless the user clearly asked for "
    "that operation; if the target or scope is unclear, ask first.\n";

struct ToolCallAcc {
    std::string id, name, args;
};

struct LlmReply {
    std::string content, reasoning, finish;
    std::vector<ToolCallAcc> calls;
    std::string error;
    Json usage;
};

enum LlmStatus { LLM_OK, LLM_CANCELLED, LLM_ERROR, LLM_CONTEXT };

// One line describing this computer, detected at runtime (XP-era APIs only).
std::string machine_line() {
    OSVERSIONINFOEXW vi;
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    GetVersionExW((OSVERSIONINFOW*)&vi);
    unsigned ver = vi.dwMajorVersion * 10 + vi.dwMinorVersion;
    bool server = vi.wProductType != VER_NT_WORKSTATION;
    const char* name = ver == 50 ? "Windows 2000"
                     : ver == 51 ? "Windows XP"
                     : ver == 52 ? (server ? "Windows Server 2003" : "Windows XP x64")
                     : ver == 60 ? (server ? "Windows Server 2008" : "Windows Vista")
                     : ver == 61 ? (server ? "Windows Server 2008 R2" : "Windows 7")
                     : ver == 62 ? "Windows 8"
                     : ver == 63 ? "Windows 8.1"
                     : ver >= 100 ? "Windows 10 or later"
                     : "Windows";
    char line[256];
    snprintf(line, sizeof line, "%s (NT %lu.%lu%s%s)", name, vi.dwMajorVersion, vi.dwMinorVersion,
             vi.szCSDVersion[0] ? ", " : "", narrow(vi.szCSDVersion).c_str());
    return line;
}

std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return utf8_head(s, n) + "...[" + std::to_string(s.size() - n) + " more bytes]";
}

}  // namespace

class AgentImpl {
public:
    Config cfg;
    Session sess;
    Mutex mu;  // guards busy, pending_text, sess (when not busy)
    bool busy = false;
    bool started = false;
    std::string pending_text;
    volatile LONG quit = 0;
    volatile LONG yolo = 0;
    HANDLE worker = nullptr;
    HANDLE work_event = nullptr;    // auto-reset
    HANDLE cancel_event = nullptr;  // manual-reset
    HANDLE approval_event = nullptr;  // auto-reset

    // events
    std::function<void(const Json&)> callback;
    Mutex qmu;
    std::deque<Json> queue;
    HANDLE queue_event = nullptr;   // manual-reset, set while queue non-empty

    // approvals
    Mutex amu;
    std::string pending_approval;
    int approval_answer = -1;
    unsigned approval_counter = 0;

    std::vector<Regex> risky;
    Json tools_json;
    std::string tools_dump;
    int cur_turn = 0;
    // conversation = open session + scratchpad + background processes
    std::string scratch;
    HANDLE scratch_lock = INVALID_HANDLE_VALUE;
    BackgroundSet bg;
    std::string machine;            // detected once at start
    double tokens_per_byte = 0;     // measured from the server's usage reports (0 = unknown)
    long long last_prompt_tokens = 0;
    unsigned call_counter = 0;

    // ------------------------------------------------------------ events
    void emit(Json ev, bool log = false) {
        if (log && sess.is_open()) {
            Json copy = ev;
            sess.add_event(cur_turn, copy);
        }
        if (callback) { callback(ev); return; }
        Lock l(qmu);
        queue.push_back(std::move(ev));
        SetEvent(queue_event);
    }
    static Json ev(const char* type) {
        Json j = Json::object();
        j.set("type", type);
        return j;
    }
    void emit_error(const std::string& msg, bool fatal, const char* where) {
        Json e = ev("error");
        e.set("message", msg).set("fatal", fatal).set("where", where);
        emit(e, true);
        if (!sess.is_open()) log_error(std::string(where) + ": " + msg);
    }
    bool cancelled() { return WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0; }

    // ------------------------------------------------------------ prompt
    std::string system_prompt() {
        std::string persona;
        if (!read_file(cfg.persona_path(), persona) || trim(persona).empty()) persona = kDefaultPersona;
        std::string memory;
        read_file(cfg.memory_path(), memory);
        if (memory.size() > 16000) memory = utf8_head(memory, 16000) + "\n[... memory file truncated here; it is larger than 16000 bytes - consider tidying it]\n";
        SYSTEMTIME st;
        GetLocalTime(&st);
        char date[64];
        // Date only: a system prompt that changes every minute defeats the server's prompt cache.
        snprintf(date, sizeof date, "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
        std::string cwd = sess.cwd.empty() ? cfg.workdir : sess.cwd;
        std::string mem_slash = to_slashes(cfg.memory_path());
        std::string s = trim(persona) + "\n\n";
        s += "# Environment\n";
        s += "Host: " + machine + "\n";
        if (!cfg.machine_note.empty()) s += "Note: " + cfg.machine_note + "\n";
        s += "Current date: " + std::string(date) + "\n";
        s += "Current working directory: " + cwd + "\n";
        s += "Scratch directory: " + scratch + " ($AGENT_SCRATCH; write temporary files there; it is deleted when "
             "this conversation ends)\n";
        if (cfg.bash_off || cfg.bash_path.empty())
            s += "Shell: no bash is available, so the bash tool will fail.\n";
        else
            s += "Shell: your bash tool runs commands through bash (git-bash / MSYS), NOT PowerShell or cmd.exe. Use POSIX "
                 "shell syntax (`ls`, `$HOME`, `&&`, `|`, single-quoted strings). MSYS-style paths like /c/Users/... work "
                 "alongside native C:\\Users\\... paths; quote paths that contain spaces. Commands have no timeout and "
                 "no input: never run interactive programs or servers in the foreground (start them in the background "
                 "with &; they are stopped when the conversation ends).\n";
        s += "\n" + std::string(kToolRules) + "\n";
        s += "# Memory\n";
        s += "You have persistent memory, carried across sessions and loaded into each new session's context: the file " +
             cfg.memory_path() + " (bash: \"" + mem_slash + "\", or $AGENT_MEMORY); change it with edit_file or bash. "
             "It is for facts that apply to EVERY session regardless of task (who the user is, environment facts, "
             "standing conventions); keep it short, and when it grows, replace or consolidate stale entries. Write "
             "entries as declarative facts, not instructions to yourself: 'User prefers concise responses' is right, "
             "'Always respond concisely' is wrong (imperative phrasing gets re-read as a directive in later sessions "
             "and can override the user's current request). A fact stale within a week does not belong there.\n";
        s += "<memory>\n" + memory + (memory.empty() || memory.back() == '\n' ? "" : "\n") + "</memory>\n";
        return s;
    }

    // ------------------------------------------------------------ LLM client
    static Json clean_messages(const std::string& system, const Json& msgs) {
        Json out = Json::array();
        Json sys = Json::object();
        sys.set("role", "system").set("content", system);
        out.push_back(sys);
        for (auto& m : msgs.items()) {
            Json c = Json::object();
            std::string role = m.get_str("role");
            c.set("role", role);
            const Json& content = m["content"];
            c.set("content", content.is_null() ? Json("") : content);
            if (m["tool_calls"].is_array() && m["tool_calls"].size()) c.set("tool_calls", m["tool_calls"]);
            // Reasoning models (DeepSeek thinking mode and others served with reasoning_content) expect the
            // reasoning of earlier assistant messages back in tool-enabled requests, so the model can continue
            // its own reasoning between tool calls. Only messages that actually carried reasoning send it.
            if (role == "assistant" && !m.get_str("reasoning_content").empty())
                c.set("reasoning_content", m["reasoning_content"]);
            if (role == "tool") {
                c.set("tool_call_id", m.get_str("tool_call_id"));
                if (m.has("name")) c.set("name", m["name"]);
            }
            out.push_back(c);
        }
        return out;
    }

    LlmStatus llm_call(const Json& messages, bool with_tools, bool stream_events, LlmReply& out) {
        Json body = Json::object();
        body.set("model", cfg.llm_model).set("messages", messages).set("stream", true);
        if (with_tools) body.set("tools", tools_json);
        if (cfg.max_tokens > 0) body.set("max_tokens", cfg.max_tokens);
        if (cfg.temperature >= 0) body.set("temperature", cfg.temperature);
        if (cfg.top_p >= 0) body.set("top_p", cfg.top_p);
        if (cfg.stream_usage) {
            Json so = Json::object();
            so.set("include_usage", true);
            body.set("stream_options", so);
        }
        std::string payload = body.dump(false);
        std::vector<std::pair<std::string, std::string>> headers = {{"Content-Type", "application/json"},
                                                                    {"Accept", "text/event-stream"}};
        if (!cfg.llm_api_key.empty()) headers.push_back({"Authorization", "Bearer " + cfg.llm_api_key});
        std::string url = cfg.llm_base_url + "/chat/completions";

        std::string last_error;
        for (int attempt = 0; attempt <= cfg.max_retries; attempt++) {
            out = LlmReply();
            bool emitted = false, done = false, stream_err = false, any = false;
            std::string raw;  // non-SSE body
            HttpResult res;
            SseParser sse([&](const std::string& data) -> bool {
                if (data == "[DONE]") { done = true; return true; }
                Json j;
                if (!Json::parse(data, j)) return true;  // ignore junk
                if (j.has("error")) {
                    const Json& e = j["error"];
                    out.error = e.is_object() ? e.get_str("message", e.dump()) : e.dump();
                    stream_err = true;
                    return false;
                }
                if (j["usage"].is_object()) out.usage = j["usage"];
                const Json& ch = j["choices"].at(0);
                if (!ch.is_object()) return true;
                const Json& d = ch["delta"];
                std::string t = d.get_str("content");
                if (!t.empty()) {
                    out.content += t;
                    any = true;
                    if (stream_events) { Json e = ev("text_delta"); e.set("text", t); emit(e); emitted = true; }
                }
                std::string r = d.get_str("reasoning_content");
                if (r.empty()) r = d.get_str("reasoning");
                if (!r.empty()) {
                    out.reasoning += r;
                    any = true;
                    if (stream_events) { Json e = ev("reasoning_delta"); e.set("text", r); emit(e); emitted = true; }
                }
                const Json& tcs = d["tool_calls"];
                for (size_t k = 0; k < tcs.size(); k++) {
                    const Json& tc = tcs.at(k);
                    long long idx = tc["index"].is_number() ? tc["index"].as_int() : (long long)k;
                    if (idx < 0 || idx > 1000) idx = (long long)k;
                    while ((long long)out.calls.size() <= idx) out.calls.push_back(ToolCallAcc());
                    ToolCallAcc& a = out.calls[(size_t)idx];
                    std::string id = tc.get_str("id");
                    if (!id.empty() && a.id.empty()) a.id = id;
                    const Json& f = tc["function"];
                    std::string nm = f.get_str("name");
                    if (!nm.empty() && a.name.empty()) a.name = nm;
                    if (f["arguments"].is_string()) a.args += f["arguments"].as_string();
                    else if (f["arguments"].is_object()) a.args += f["arguments"].dump();
                    any = true;
                }
                std::string fr = ch.get_str("finish_reason");
                if (!fr.empty()) out.finish = fr;
                return true;
            });
            HttpOptions opt;
            opt.connect_timeout_ms = cfg.connect_timeout_s * 1000;
            opt.idle_timeout_ms = cfg.idle_timeout_s * 1000;
            opt.cancel_event = cancel_event;
            http_request("POST", url, headers, payload, opt,
                         [&](const char* p, size_t n) -> bool {
                             if (lower(res.content_type).find("text/event-stream") != std::string::npos) return sse.feed(p, n);
                             raw.append(p, n);
                             return true;
                         },
                         res);
            if (res.cancelled && cancelled()) return LLM_CANCELLED;
            bool is_sse = lower(res.content_type).find("text/event-stream") != std::string::npos;
            if (res.status >= 200 && res.status < 300 && !is_sse && res.complete && !raw.empty()) {
                // server ignored stream=true: plain JSON response
                Json j;
                if (Json::parse(raw, j) && j["choices"].at(0).is_object()) {
                    const Json& m = j["choices"].at(0)["message"];
                    out.content = m.get_str("content");
                    out.reasoning = m.get_str("reasoning_content");
                    for (auto& tc : m["tool_calls"].items()) {
                        ToolCallAcc a;
                        a.id = tc.get_str("id");
                        a.name = tc["function"].get_str("name");
                        a.args = tc["function"]["arguments"].is_string() ? tc["function"]["arguments"].as_string() : tc["function"]["arguments"].dump();
                        out.calls.push_back(a);
                    }
                    out.finish = j["choices"].at(0).get_str("finish_reason");
                    if (j["usage"].is_object()) out.usage = j["usage"];
                    if (stream_events && !out.reasoning.empty()) { Json e = ev("reasoning_delta"); e.set("text", out.reasoning); emit(e); }
                    if (stream_events && !out.content.empty()) { Json e = ev("text_delta"); e.set("text", out.content); emit(e); }
                    return LLM_OK;
                }
                last_error = "unparsable non-streaming response: " + clip(raw, 300);
            } else if (res.status >= 200 && res.status < 300) {
                if (is_sse && res.complete && !done && !stream_err) sse.finish();
                if (!stream_err && (done || !out.finish.empty() || (res.complete && any))) return LLM_OK;
                if (stream_err) last_error = "error in stream: " + out.error;
                else if (!res.error.empty()) last_error = "stream broken: " + res.error;
                else last_error = "stream ended without data";
            } else if (res.status == 0) {
                last_error = res.connect_failed ? "connection failed: " + res.error : "request failed: " + res.error;
            } else {
                std::string b = clip(res.body, 500);
                std::string lb = lower(b);
                last_error = "HTTP " + std::to_string(res.status) + ": " + b;
                bool retryable = res.status == 408 || res.status == 429 || res.status >= 500;
                if ((res.status == 400 || res.status == 413) &&
                    (lb.find("context") != std::string::npos || lb.find("too long") != std::string::npos ||
                     lb.find("maximum") != std::string::npos || lb.find("exceed") != std::string::npos)) {
                    out.error = last_error;
                    return LLM_CONTEXT;
                }
                if (!retryable) { out.error = last_error; return LLM_ERROR; }
            }
            if (attempt == cfg.max_retries) break;
            unsigned delay = 1000u << (attempt < 5 ? attempt : 5);
            if (delay > 30000) delay = 30000;
            Json e = ev("retry");
            e.set("attempt", attempt + 1).set("max_retries", cfg.max_retries).set("delay_ms", delay).set("error", last_error)
                .set("discard_partial", emitted);
            emit(e, true);
            if (WaitForSingleObject(cancel_event, delay) == WAIT_OBJECT_0) return LLM_CANCELLED;
        }
        out.error = last_error + " (gave up after " + std::to_string(cfg.max_retries + 1) + " attempts)";
        return LLM_ERROR;
    }

    // ------------------------------------------------------------ compression
    size_t request_bytes(const std::string& system, const Json& msgs) {
        return system.size() + tools_dump.size() + msgs.dump(false).size();
    }
    // Uses the tokens-per-byte ratio measured from the server's last usage report when available,
    // otherwise bytes / chars_per_token.
    long estimate_tokens(const std::string& system, const Json& msgs) {
        double b = (double)request_bytes(system, msgs);
        return (long)(tokens_per_byte > 0 ? b * tokens_per_byte : b / cfg.chars_per_token);
    }

    // Returns false if cancelled.
    bool maybe_compress(const std::string& system, bool force) {
        long before = estimate_tokens(system, sess.messages);
        long limit = (long)(cfg.context_length * cfg.compress_threshold);
        if (!force && before <= limit) return true;
        Json& msgs = sess.messages;
        size_t n = msgs.size();
        if (n < 2) return true;
        // keep the most recent messages within keep_recent * context_length
        size_t keep_bytes = (size_t)(cfg.context_length * cfg.keep_recent * cfg.chars_per_token);
        size_t acc = 0, cut = n;
        while (cut > 0) {
            size_t sz = msgs.at(cut - 1).dump(false).size();
            if (acc + sz > keep_bytes && cut < n) break;
            acc += sz;
            cut--;
        }
        if (cut == 0) cut = 1;  // keep at least... everything fits: still compress the oldest message when forced
        while (cut < n && msgs.at(cut).get_str("role") == "tool") cut++;  // never start the kept part with a tool result
        if (cut >= n) {
            // everything after the cut was tool results: keep from the last assistant message instead
            size_t k = n;
            while (k > 0 && msgs.at(k - 1).get_str("role") != "assistant") k--;
            cut = k > 0 ? k - 1 : 0;
        }
        if (cut == 0 || (cut == 1 && msgs.at(0).get_bool("is_summary"))) return true;  // nothing worth compressing

        // transcript of the old part
        size_t budget = (size_t)(cfg.context_length * 0.5 * cfg.chars_per_token);
        size_t per_msg = budget / cut;
        if (per_msg > 3000) per_msg = 3000;
        if (per_msg < 200) per_msg = 200;
        std::string transcript;
        for (size_t i = 0; i < cut; i++) {
            const Json& m = msgs.at(i);
            std::string role = m.get_str("role");
            std::string c = m["content"].is_string() ? m["content"].as_string() : "";
            if (m.get_bool("is_summary")) transcript += "[EARLIER SUMMARY]\n" + clip(c, per_msg * 2) + "\n\n";
            else if (role == "user") transcript += "USER: " + clip(c, per_msg) + "\n\n";
            else if (role == "assistant") {
                if (!c.empty()) transcript += "ASSISTANT: " + clip(c, per_msg) + "\n\n";
                for (auto& tc : m["tool_calls"].items())
                    transcript += "ASSISTANT CALLED " + tc["function"].get_str("name") + "(" + clip(tc["function"].get_str("arguments"), per_msg / 2) + ")\n";
            } else if (role == "tool") transcript += "TOOL RESULT (" + m.get_str("name") + "): " + clip(c, per_msg) + "\n\n";
        }
        if (transcript.size() > budget) transcript = utf8_head(transcript, budget) + "\n[transcript truncated]\n";

        long words = cfg.context_length / 8;
        if (words > 800) words = 800;
        if (words < 60) words = 60;
        Json req = Json::array();
        Json sys = Json::object();
        sys.set("role", "system").set("content",
            "You compress conversation history for an AI agent (agent context compression). Summarize the transcript "
            "so the agent can continue without it. Keep the user's goals and requests, decisions, important facts, file "
            "paths, commands run and their results, errors, and what is still to be done. Plain text, at most about " +
            std::to_string(words) + " words.");
        Json usr = Json::object();
        usr.set("role", "user").set("content", "Summarize this conversation transcript:\n\n" + transcript);
        req.push_back(sys);
        req.push_back(usr);
        LlmReply rep;
        std::string method = "llm";
        LlmStatus st = llm_call(req, false, false, rep);
        if (st == LLM_CANCELLED) return false;
        std::string summary = trim(rep.content);
        if (st != LLM_OK || summary.empty()) {
            emit_error("compression summary call failed (" + (rep.error.empty() ? std::string("empty summary") : rep.error) +
                       "); using a truncated transcript instead", false, "compression");
            method = "fallback";
            summary = utf8_head(transcript, (size_t)(cfg.context_length * 0.15 * cfg.chars_per_token));
        }
        // keep the latest user request verbatim if it falls into the compressed part
        size_t last_user = n;
        for (size_t i = n; i > 0; i--)
            if (msgs.at(i - 1).get_str("role") == "user" && !msgs.at(i - 1).get_bool("is_summary")) { last_user = i - 1; break; }
        std::string content = "[Summary of the earlier conversation; older messages were compressed automatically to save context]\n" + summary;
        if (last_user < cut) content += "\n\n[The user's current request, verbatim]\n" + clip(msgs.at(last_user).get_str("content"), 4000);
        Json smsg = Json::object();
        smsg.set("role", "user").set("content", content).set("is_summary", true);
        Json info = Json::object();
        info.set("before_tokens", before).set("method", method);
        sess.apply_compaction(cur_turn, cut, smsg, info);
        long after = estimate_tokens(system, sess.messages);
        Json e = ev("compression");
        e.set("turn", cur_turn).set("before_tokens", before).set("after_tokens", after).set("summarized_messages", (long long)cut)
            .set("kept_messages", (long long)(sess.messages.size() - 1)).set("method", method).set("context_length", cfg.context_length)
            .set("estimate", tokens_per_byte > 0 ? "usage" : "chars");
        emit(e, true);
        return true;
    }

    // ------------------------------------------------------------ approvals
    bool request_approval(const std::string& tool_call_id, const std::string& command, const std::string& reason) {
        if (yolo) return true;
        std::string id;
        {
            Lock l(amu);
            id = "ap" + std::to_string(++approval_counter);
            pending_approval = id;
            approval_answer = -1;
            ResetEvent(approval_event);
        }
        Json e = ev("approval_request");
        e.set("request_id", id).set("tool_call_id", tool_call_id).set("tool", "bash").set("command", command).set("reason", reason);
        emit(e, true);
        HANDLE hs[2] = {approval_event, cancel_event};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        bool ok;
        {
            Lock l(amu);
            ok = w == WAIT_OBJECT_0 && approval_answer == 1;
            pending_approval.clear();
        }
        Json r = ev("approval_resolved");
        r.set("request_id", id).set("approved", ok).set("cancelled", w != WAIT_OBJECT_0);
        emit(r, true);
        return ok;
    }

    // ------------------------------------------------------------ turn
    void run_turn(const std::string& text) {
        int turn = sess.begin_turn();
        cur_turn = turn;
        Json st = ev("turn_started");
        st.set("turn", turn).set("session", sess.id);
        emit(st);
        Json um = Json::object();
        um.set("role", "user").set("content", text);
        sess.add_message(turn, um);

        std::string status = "done", final_text, error_msg;
        long long ntools = 0;
        std::string cfg_problem = cfg.check();
        if (!cfg_problem.empty()) {
            status = "error";
            error_msg = cfg_problem;
            emit_error(cfg_problem, true, "config");
        }
        bool context_retry = false;
        std::string cwd = sess.cwd.empty() ? cfg.workdir : sess.cwd;

        while (cfg_problem.empty()) {
            if (cancelled()) { status = "cancelled"; break; }
            std::string system = system_prompt();
            if (!maybe_compress(system, context_retry)) { status = "cancelled"; break; }
            LlmReply rep;
            size_t req_bytes = request_bytes(system, sess.messages);
            LlmStatus ls = llm_call(clean_messages(system, sess.messages), true, true, rep);
            long long pt = rep.usage.get_int("prompt_tokens", 0);
            if (ls == LLM_OK && pt > 0 && req_bytes > 0) {
                tokens_per_byte = (double)pt / (double)req_bytes;
                last_prompt_tokens = pt;
                Json ue = ev("usage");
                ue.set("prompt_tokens", pt).set("completion_tokens", rep.usage.get_int("completion_tokens", 0))
                    .set("total_tokens", rep.usage.get_int("total_tokens", 0)).set("context_length", cfg.context_length);
                emit(ue);
            }
            if (ls == LLM_CONTEXT && !context_retry) {
                emit_error("server says the context is too long; compressing and retrying: " + rep.error, false, "llm");
                context_retry = true;
                continue;
            }
            context_retry = false;
            if (ls == LLM_CANCELLED) {
                if (!rep.content.empty()) {
                    Json am = Json::object();
                    am.set("role", "assistant").set("content", rep.content + "\n[cancelled by the user]").set("is_cancelled_partial", true);
                    sess.add_message(turn, am);
                }
                status = "cancelled";
                final_text = rep.content;
                break;
            }
            if (ls != LLM_OK) {
                status = "error";
                error_msg = rep.error;
                emit_error("LLM request failed: " + rep.error, true, "llm");
                break;
            }
            // assign ids, store the assistant message
            Json am = Json::object();
            am.set("role", "assistant").set("content", rep.content);
            if (!rep.reasoning.empty()) am.set("reasoning_content", rep.reasoning);
            std::vector<ToolCallAcc> calls;
            for (auto& c : rep.calls) if (!c.name.empty() || !c.args.empty()) calls.push_back(c);
            if (!calls.empty()) {
                Json tcs = Json::array();
                for (auto& c : calls) {
                    if (c.id.empty()) c.id = "call_" + std::to_string(turn) + "_" + std::to_string(++call_counter);
                    Json f = Json::object();
                    f.set("name", c.name).set("arguments", c.args.empty() ? std::string("{}") : c.args);
                    Json tc = Json::object();
                    tc.set("id", c.id).set("type", "function").set("function", f);
                    tcs.push_back(tc);
                }
                am.set("tool_calls", tcs);
            }
            sess.add_message(turn, am);
            if (calls.empty()) { final_text = rep.content; break; }

            for (auto& c : calls) {
                Json targs;
                std::string perr;
                bool args_ok = Json::parse(c.args.empty() ? std::string("{}") : c.args, targs, &perr);
                if (args_ok && targs.is_string()) args_ok = Json::parse(targs.as_string(), targs, &perr);  // double-encoded
                Json se = ev("tool_call_started");
                se.set("id", c.id).set("name", c.name).set("args", args_ok ? targs : Json(c.args));
                emit(se);
                ToolResult tr;
                unsigned long t0 = tick_ms();
                if (cancelled()) {
                    tr.ok = false;
                    tr.cancelled = true;
                    tr.content = "Not run: the turn was cancelled by the user.";
                    tr.summary = "not run (cancelled)";
                } else if (!args_ok) {
                    tr.ok = false;
                    tr.content = "Error: the arguments are not valid JSON (" + perr + "). Received: " + clip(c.args, 500);
                    tr.summary = "invalid JSON arguments";
                } else {
                    ToolContext ctx;
                    ctx.cfg = &cfg;
                    ctx.scratch = scratch;
                    ctx.background = &bg;
                    ctx.turn = turn;
                    ctx.cancel_event = cancel_event;
                    ctx.cwd = &cwd;
                    ctx.tool_call_id = c.id;
                    ctx.risky = &risky;
                    ctx.yolo = yolo != 0;
                    std::string cid = c.id;
                    ctx.approve = [this, cid](const std::string& cmd, const std::string& why) { return request_approval(cid, cmd, why); };
                    tr = run_tool(c.name, targs, ctx);
                    if (!cwd.empty() && cwd != (sess.cwd.empty() ? cfg.workdir : sess.cwd)) sess.set_cwd(turn, cwd);
                }
                ntools++;
                Json tm = Json::object();
                tm.set("role", "tool").set("tool_call_id", c.id).set("name", c.name).set("content", tr.content);
                sess.add_message(turn, tm);
                Json fe = ev("tool_finished");
                fe.set("id", c.id).set("name", c.name).set("ok", tr.ok)
                    .set("exit_code", tr.has_exit_code ? Json(tr.exit_code) : Json())
                    .set("cancelled", tr.cancelled).set("denied", tr.denied).set("summary", tr.summary)
                    .set("output_file", tr.output_file.empty() ? Json() : Json(tr.output_file))
                    .set("duration_ms", (long long)(tick_ms() - t0));
                if (sess.is_open()) sess.add_event(turn, fe);  // logged without the full result (that is in the tool message)
                fe.set("result", tr.content);
                emit(fe);
            }
            if (cancelled()) { status = "cancelled"; break; }
        }
        Json fin = ev("turn_finished");
        fin.set("turn", turn).set("session", sess.id).set("status", status).set("text", final_text).set("tool_calls", ntools);
        if (!error_msg.empty()) fin.set("error", error_msg);
        sess.add_event(turn, fin);
        {
            Lock l(mu);
            busy = false;
        }
        emit(fin);
    }

    // ------------------------------------------------------------ conversation lifecycle
    static std::string lock_path(const std::string& dir) { return path_join(dir, ".lock"); }

    // Remove scratch dirs left by crashed runs; skip dirs whose lock file another instance holds open.
    static void cleanup_stale_scratch(const std::string& root) {
        for (auto& name : list_subdirs(root)) {
            std::string dir = path_join(root, name);
            HANDLE h = CreateFileW(widen(lock_path(dir)).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) continue;  // owned by a live agent
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            bool ok = delete_tree(dir);
            log_error("startup: removed stale scratch dir " + dir + (ok ? "" : " (incomplete: files still in use)"));
        }
    }

    // Called with mu held, right after sess was created or opened.
    bool begin_conversation(std::string* err) {
        std::string root = cfg.scratch_root();
        make_dirs(root);
        std::string dir = path_join(root, sess.id);
        if (dir_exists(dir)) {
            HANDLE h = CreateFileW(widen(lock_path(dir)).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
                dir += "-" + std::to_string(GetCurrentProcessId());  // the same session is open in another instance
            } else {
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            delete_tree(dir);  // a resumed session gets a fresh, empty scratchpad
        }
        if (!make_dirs(dir)) { if (err) *err = "cannot create scratch dir " + dir; return false; }
        scratch_lock = CreateFileW(widen(lock_path(dir)).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_HIDDEN, nullptr);
        scratch = dir;
        Json r = Json::object();
        r.set("t", "conversation_start").set("scratch", dir).set("pid", (long long)GetCurrentProcessId());
        sess.append(r);
        return true;
    }

    // Kill background processes, delete the scratchpad, close the session. Called with mu held
    // and no turn running (or after the worker has stopped).
    void end_conversation() {
        if (!sess.is_open() && scratch.empty() && bg.jobs.empty() && bg.orphan_roots.empty()) return;
        long running = bg_prune(bg);
        bg_kill_all(bg);
        if (scratch_lock != INVALID_HANDLE_VALUE) CloseHandle(scratch_lock);
        scratch_lock = INVALID_HANDLE_VALUE;
        bool gone = delete_tree(scratch);
        if (!gone) log_error("could not delete scratch dir " + scratch + " (files still in use)");
        if (sess.is_open()) {
            Json r = Json::object();
            r.set("t", "conversation_end").set("killed_background", (long long)running).set("scratch_deleted", gone);
            sess.append(r);
        }
        scratch.clear();
        sess = Session();
    }

    static unsigned __stdcall worker_main(void* p) {
        AgentImpl* a = (AgentImpl*)p;
        while (true) {
            WaitForSingleObject(a->work_event, INFINITE);
            if (a->quit) break;
            std::string text;
            {
                Lock l(a->mu);
                if (!a->busy) continue;
                text = a->pending_text;
            }
            try {
                a->run_turn(text);
            } catch (const std::exception& e) {
                log_error(std::string("turn crashed: ") + e.what());
                {
                    Lock l(a->mu);
                    a->busy = false;
                }
                Json fin = ev("turn_finished");
                fin.set("turn", a->cur_turn).set("status", "error").set("error", std::string("internal error: ") + e.what());
                a->emit(fin);
            }
        }
        return 0;
    }
};

// ---------------------------------------------------------------- Agent

Agent::Agent() : impl_(new AgentImpl) {}
Agent::~Agent() {
    shutdown();
    delete impl_;
}

void Agent::set_event_callback(std::function<void(const Json&)> cb) { impl_->callback = std::move(cb); }

bool Agent::start(const Config& cfg, std::string* err) {
    AgentImpl* a = impl_;
    if (a->started) return true;
    a->cfg = cfg;
    a->yolo = cfg.yolo ? 1 : 0;
    if (!make_dirs(cfg.data_dir)) { if (err) *err = "cannot create data dir " + cfg.data_dir; return false; }
    set_error_log_path(path_join(cfg.data_dir, "agent-error.log"));
    make_dirs(cfg.sessions_dir());
    make_dirs(cfg.workdir);
    AgentImpl::cleanup_stale_scratch(cfg.scratch_root());
    if (!file_exists(cfg.persona_path())) write_file(cfg.persona_path(), kDefaultPersona);
    if (!file_exists(cfg.memory_path())) write_file(cfg.memory_path(), kDefaultMemory);
    if (!file_exists(cfg.patterns_path()))
        write_file(cfg.patterns_path(),
                   "# Extra approval patterns for risky bash commands, one per line (case-insensitive regex:\n"
                   "# . [] [^] \\s \\d \\w \\b ^ $ ( ) | * + ?). Built-in patterns are always active.\n"
                   "# Example: \\bnpm\\s+publish\\b\n");
    std::vector<std::string> perrs;
    a->risky = load_risky_patterns(cfg.patterns_path(), &perrs);
    for (auto& e : perrs) log_error(e);
    a->machine = machine_line();
    a->tools_json = tool_schemas();
    a->tools_dump = a->tools_json.dump();
    proc_init();
    winsock_init();
    a->work_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    a->cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    a->approval_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    a->queue_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unsigned tid;
    a->worker = (HANDLE)_beginthreadex(nullptr, 0, &AgentImpl::worker_main, a, 0, &tid);
    if (!a->worker) { if (err) *err = "cannot create worker thread"; return false; }
    a->started = true;
    return true;
}

void Agent::shutdown() {
    AgentImpl* a = impl_;
    if (!a->started) return;
    a->quit = 1;
    SetEvent(a->cancel_event);
    SetEvent(a->work_event);
    WaitForSingleObject(a->worker, 15000);
    CloseHandle(a->worker);
    a->started = false;
    Lock l(a->mu);
    a->end_conversation();
}

bool Agent::next_event(Json& out, unsigned timeout_ms) {
    AgentImpl* a = impl_;
    for (int pass = 0; pass < 2; pass++) {
        {
            Lock l(a->qmu);
            if (!a->queue.empty()) {
                out = std::move(a->queue.front());
                a->queue.pop_front();
                if (a->queue.empty()) ResetEvent(a->queue_event);
                return true;
            }
        }
        if (pass == 0) WaitForSingleObject(a->queue_event, timeout_ms);
    }
    return false;
}

bool Agent::start_turn(const std::string& text, std::string* err) {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (!a->started) { if (err) *err = "agent not started"; return false; }
    if (a->busy) { if (err) *err = "busy: a turn is already running"; return false; }
    if (!a->sess.is_open()) {
        Json meta = Json::object();
        meta.set("app", "agent").set("model", a->cfg.llm_model);
        if (!a->sess.create(a->cfg.sessions_dir(), meta, err)) return false;
        if (!a->begin_conversation(err)) return false;
    }
    a->busy = true;
    a->pending_text = text;
    ResetEvent(a->cancel_event);
    SetEvent(a->work_event);
    return true;
}

void Agent::cancel() {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) SetEvent(a->cancel_event);
}

bool Agent::answer_approval(const std::string& id, bool approve) {
    AgentImpl* a = impl_;
    Lock l(a->amu);
    if (a->pending_approval.empty() || a->pending_approval != id) return false;
    a->approval_answer = approve ? 1 : 0;
    SetEvent(a->approval_event);
    return true;
}

std::string Agent::pending_approval() {
    Lock l(impl_->amu);
    return impl_->pending_approval;
}

bool Agent::new_session(std::string* id, std::string* err) {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) { if (err) *err = "busy"; return false; }
    Json meta = Json::object();
    meta.set("app", "agent").set("model", a->cfg.llm_model);
    a->end_conversation();
    if (!a->sess.create(a->cfg.sessions_dir(), meta, err)) return false;
    if (!a->begin_conversation(err)) return false;
    if (id) *id = a->sess.id;
    return true;
}

bool Agent::resume_session(const std::string& id0, std::string* err) {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) { if (err) *err = "busy"; return false; }
    std::string id = id0;
    if (id == "last") {
        id = Session::latest_id(a->cfg.sessions_dir());
        if (id.empty()) { if (err) *err = "no sessions yet"; return false; }
    }
    Session s;
    if (!s.open(a->cfg.sessions_dir(), id, err)) return false;
    a->end_conversation();
    a->sess = s;
    return a->begin_conversation(err);
}

bool Agent::close_session(std::string* err) {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) { if (err) *err = "busy"; return false; }
    a->end_conversation();
    return true;
}

std::string Agent::scratch_dir() {
    Lock l(impl_->mu);
    return impl_->scratch;
}

std::vector<SessionInfo> Agent::list_sessions() { return Session::list(impl_->cfg.sessions_dir()); }

bool Agent::undo_last_turn(Json* report, std::string* err) {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) { if (err) *err = "busy"; return false; }
    if (!a->sess.is_open()) { if (err) *err = "no session open"; return false; }
    Json r;
    if (!a->sess.undo_last(r, err)) return false;
    if (report) *report = r;
    return true;
}

Json Agent::history() {
    AgentImpl* a = impl_;
    Lock l(a->mu);
    if (a->busy) return Json::array();
    return a->sess.messages;
}

std::string Agent::session_id() {
    Lock l(impl_->mu);
    return impl_->sess.id;
}
bool Agent::busy() {
    Lock l(impl_->mu);
    return impl_->busy;
}
void Agent::set_yolo(bool on) { InterlockedExchange(&impl_->yolo, on ? 1 : 0); }
bool Agent::yolo() { return impl_->yolo != 0; }
const Config& Agent::config() const { return impl_->cfg; }

}  // namespace ag
