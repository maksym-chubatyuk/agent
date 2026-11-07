// INI-style configuration. See NOTES.md / agent.ini.template for the format.
#pragma once
#include <string>
#include <map>

namespace ag {

// Find bash.exe: PATH first, then common Git for Windows folders. "" if not found.
std::string detect_bash();

struct Config {
    // [llm]
    // No setup-specific defaults: endpoint, model, Firecrawl and bash come only from the config file.
    std::string llm_base_url;          // e.g. http://localhost:8080/v1 (required)
    std::string llm_model;             // required
    std::string llm_api_key;          // optional; sent as "Authorization: Bearer ..." when set
    long context_length = 32768;      // tokens
    double compress_threshold = 0.75; // compress when estimate > context_length * threshold
    double keep_recent = 0.35;        // fraction of context_length kept verbatim after compression
    double chars_per_token = 3.5;     // estimate before the server reported usage: tokens = bytes / chars_per_token
    bool stream_usage = true;         // ask for usage in the stream (stream_options.include_usage)
    long max_tokens = 0;              // 0 = don't send
    double temperature = -1;          // <0 = don't send
    int max_retries = 6;
    int connect_timeout_s = 10;
    int idle_timeout_s = 600;         // max silence on an open LLM connection

    // [firecrawl]
    std::string firecrawl_url;        // e.g. http://localhost:3002 (web tools fail with a clear error if unset)
    int scrape_timeout_ms = 60000;
    int search_limit = 5;

    // [tools]
    std::string bash_path;            // empty = auto-detect (PATH, then common Git for Windows folders)
    bool bash_autodetected = false;
    std::string workdir;              // initial cwd for bash and relative paths (default: <data_dir>\work)
    std::string path_extra;           // prepended to the child PATH
    long spill_threshold = 16000;     // bytes; longer output is saved to a file
    long spill_head = 4000;           // bytes of head returned with a spilled output
    long spill_tail = 1000;           // bytes of tail returned with spilled bash output
    long read_limit = 500;            // default read_file line count
    long read_max_bytes = 60000;      // cap on read_file result size

    // [agent]
    std::string data_dir;             // default: <exe dir>\data
    std::string persona_file = "persona.md";      // relative to data_dir
    std::string memory_file = "MEMORY.md";        // relative to data_dir
    std::string approval_patterns_file = "approval_patterns.txt";  // relative to data_dir; extra patterns
    bool yolo = false;

    std::string source_path;          // where it was loaded from
    std::map<std::string, std::string> raw;  // all "section.key" values

    // Loads path (missing file = defaults). Resolves relative paths. Returns false on parse error.
    // overrides: "section.key" -> value, applied on top of the file (e.g. from --set on the command line).
    bool load(const std::string& path, std::string* err, const std::map<std::string, std::string>* overrides = nullptr);
    void finalize();  // resolve defaults/relative paths (called by load)

    std::string machine_note;         // optional extra line about the computer for the system prompt

    // Problems that make turns impossible (missing base_url/model); "" if none.
    std::string check() const;

    std::string persona_path() const;
    std::string memory_path() const;
    std::string patterns_path() const;
    std::string sessions_dir() const;
    std::string scratch_root() const;  // <data_dir>\scratch (one subfolder per conversation)
};

}  // namespace ag
