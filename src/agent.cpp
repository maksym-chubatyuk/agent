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

const char* kDefaultPersona =
    "You are a capable and careful assistant agent running on a Windows computer.\n"
    "You get things done with your tools: web search, fetching pages, bash commands and file editing.\n"
    "Work step by step, check results, and keep answers concise. The machine may be old and slow:\n"
    "prefer light commands and avoid huge outputs.\n";

const char* kDefaultMemory = "# Agent memory\n\n";

// Tool-use guidance for the system prompt. Taken from Hermes Agent's agent/prompt_builder.py
// (PARALLEL_TOOL_CALL_GUIDANCE and OPENAI_MODEL_EXECUTION_GUIDANCE, which Hermes applies to all model
// families), with tool names mapped to this agent's tools and lines about tools it
// doesn't have removed.
const char* kToolGuidance =
    "# Parallel tool calls\n"
    "When you need several pieces of information that don't depend on each other, request them together in a "
    "single response instead of one tool call per turn. Independent reads, searches, web fetches, and "
    "read-only commands should be batched into the same assistant turn - batching avoids resending the whole "
    "conversation on every extra round-trip.\n"
    "Only serialize calls when a later call genuinely depends on an earlier call's result (e.g. you must "
    "read a file before you can patch it). When in doubt and the calls are independent, batch them.\n"
    "\n"
    "# Execution discipline\n"
    "<tool_persistence>\n"
    "- Use tools whenever they improve correctness, completeness, or grounding.\n"
    "- Do not stop early when another tool call would materially improve the result.\n"
    "- If a tool returns empty, partial, or suspiciously narrow results, retry with a broader or different query or "
    "strategy before concluding.\n"
    "- Keep calling tools until: (1) the task is complete, AND (2) you have verified the result.\n"
    "</tool_persistence>\n\n"
    "<mandatory_tool_use>\n"
    "NEVER answer these from memory or mental computation - ALWAYS use a tool:\n"
    "- Arithmetic, math, calculations -> use bash\n"
    "- Hashes, encodings, checksums -> use bash (e.g. sha256sum, base64)\n"
    "- Current time, date, timezone -> use bash (e.g. date)\n"
    "- System state: OS, CPU, memory, disk, ports, processes -> use bash\n"
    "- File contents, sizes, line counts -> use read_file or bash\n"
    "- Current facts (weather, news, versions) -> use web_search and web_scrape\n"
    "Your memory describes the USER, not the system you are running on. The execution environment may "
    "differ from what the memory says about their personal setup.\n"
    "</mandatory_tool_use>\n\n"
    "<act_dont_ask>\n"
    "When a question has an obvious default interpretation, act on it immediately instead of asking for clarification. "
    "Examples:\n"
    "- 'Is port 443 open?' -> check THIS machine (don't ask 'open where?')\n"
    "- 'What OS am I running?' -> check the live system (don't use memory)\n"
    "- 'What time is it?' -> run `date` (don't guess)\n"
    "Only ask for clarification when the ambiguity genuinely changes what tool you would call.\n"
    "</act_dont_ask>\n\n"
    "<prerequisite_checks>\n"
    "- Before taking an action, check whether prerequisite discovery, lookup, or context-gathering steps are needed.\n"
    "- Do not skip prerequisite steps just because the final action seems obvious.\n"
    "- If a task depends on output from a prior step, resolve that dependency first.\n"
    "</prerequisite_checks>\n\n"
    "<verification>\n"
    "Before finalizing your response:\n"
    "- Correctness: does the output satisfy every stated requirement?\n"
    "- Grounding: are factual claims backed by tool outputs or provided context?\n"
    "- Formatting: does the output match the requested format or schema?\n"
    "- Safety: if the next step has side effects (file writes, commands), confirm scope before executing.\n"
    "- Completion: 'done' means every named acceptance criterion is verified - never a plausible subset. Completing "
    "your plan is not itself the answer; the requested output must appear in your response.\n"
    "</verification>\n\n"
    "<literal_preservation>\n"
    "- Preserve identifiers, commands, and values exactly as given - never 'repair' or normalize a token that fails a "
    "stated format. A successful lookup does not validate a malformed source token; validate format first, then look "
    "up.\n"
    "</literal_preservation>\n\n"
    "<missing_context>\n"
    "- If required context is missing, do NOT guess or hallucinate an answer.\n"
    "- Use the appropriate lookup tool when missing information is retrievable (read_file, bash, web_search, "
    "web_scrape).\n"
    "- Ask a clarifying question only when the information cannot be retrieved by tools.\n"
    "- If you must proceed with incomplete information, label assumptions explicitly.\n"
    "</missing_context>\n";

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
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    MEMORYSTATUS ms;
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    std::string cpu;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[256];
        DWORD sz = sizeof buf, type = 0;
        if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS && type == REG_SZ)
            cpu = trim(narrow(std::wstring(buf, wcsnlen(buf, sz / sizeof(wchar_t)))));
        RegCloseKey(k);
    }
    char line[512];
    snprintf(line, sizeof line, "Windows NT %lu.%lu build %lu%s%s, %lu logical CPU(s)%s%s, %lu MB RAM, ANSI code page %u, OEM code page %u",
             vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, vi.szCSDVersion[0] ? " " : "",
             narrow(vi.szCSDVersion).c_str(), si.dwNumberOfProcessors, cpu.empty() ? "" : ", ", cpu.c_str(),
             (unsigned long)(ms.dwTotalPhys / (1024 * 1024)), GetACP(), GetOEMCP());
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
        snprintf(date, sizeof date, "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        std::string cwd = sess.cwd.empty() ? cfg.workdir : sess.cwd;
        std::string mem_slash = to_slashes(cfg.memory_path());
        std::string s = trim(persona) + "\n\n";
        s += "# Environment\n";
        s += "- Computer: " + machine + ".\n";
        if (!cfg.machine_note.empty()) s += "- Note about this computer: " + cfg.machine_note + "\n";
        s += "- Local date/time: " + std::string(date) + "\n";
        s += "- The bash tool runs Git Bash (" + (cfg.bash_off ? std::string("turned off") : cfg.bash_path.empty() ? std::string("not found!") : cfg.bash_path) +
             "). Both C:/path and /c/path forms work; quote paths with spaces.\n";
        s += "- Current working directory: " + cwd + " (bash `cd` persists between calls; relative file paths resolve here).\n";
        s += "- Data directory: " + cfg.data_dir + ".\n";
        s += "- Scratchpad for this conversation: " + scratch + " (bash: $AGENT_SCRATCH). Put temporary files there. "
             "Long tool output and scraped pages are saved there too. It is deleted when the conversation ends.\n";
        s += "- Background processes you start in bash (cmd &) keep running for the rest of this conversation (their "
             "output goes to a log file in the scratchpad) and are killed when the conversation ends.\n";
        s += "- Commands have no timeout and stdin is empty: never run interactive programs or servers in the foreground.\n";
        s += "- The user can undo a turn: that removes it from the conversation history only; files stay as they are.\n";
        s += "- Some commands (deleting recursively, registry edits, shutdown, writing to system folders...) need the user's approval.\n\n";
        s += std::string(kToolGuidance) + "\n";
        s += "# Persistent memory\n";
        s += "Your persistent memory file is " + cfg.memory_path() + " (bash: \"" + mem_slash + "\", also $AGENT_MEMORY).\n";
        s += "It is loaded into this prompt at the start of every turn and survives across sessions. When you learn something "
             "worth remembering (user preferences, facts about this machine, lessons learned), append to or edit this file "
             "yourself, e.g. with bash: echo \"- fact\" >> \"" + mem_slash + "\" (or edit_file). Keep it short and factual.\n";
        s += "Current memory file contents:\n<memory>\n" + memory + (memory.empty() || memory.back() == '\n' ? "" : "\n") + "</memory>\n";
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
