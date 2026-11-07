// The six model tools: web_search, web_scrape, bash, read_file, write_file, edit_file.
#pragma once
#include "json.h"
#include "config.h"
#include "regex_lite.h"
#include "proc.h"
#include <string>
#include <vector>
#include <functional>
#include <windows.h>

namespace ag {


struct ToolContext {
    const Config* cfg = nullptr;
    int turn = 0;
    HANDLE cancel_event = nullptr;
    std::string* cwd = nullptr;                 // current working directory (Windows form), updated by bash
    std::string scratch;                        // this conversation's scratchpad (spill files, scripts, logs)
    BackgroundSet* background = nullptr;        // background processes of this conversation
    std::string tool_call_id;
    // Ask the user to approve a risky command. Blocks; returns true if approved.
    std::function<bool(const std::string& command, const std::string& reason)> approve;
    const std::vector<Regex>* risky = nullptr;  // approval patterns
    bool yolo = false;
};

struct ToolResult {
    std::string content;        // text returned to the model
    bool ok = true;
    bool has_exit_code = false;
    int exit_code = 0;
    bool cancelled = false;
    bool denied = false;
    std::string summary;        // one-line summary for the frontend
    std::string output_file;    // full output file when spilled / scraped
};

Json tool_schemas();
ToolResult run_tool(const std::string& name, const Json& args, ToolContext& ctx);

// Approval patterns: built-in defaults plus one regex per line from the patterns file.
std::vector<std::string> default_risky_patterns();
std::vector<Regex> load_risky_patterns(const std::string& extra_file, std::vector<std::string>* errors);
// Returns the matching pattern text, or "" if the command is not flagged.
std::string check_risky(const std::string& command, const std::vector<Regex>& pats);

// Strip quote characters and collapse whitespace (Firecrawl misbehaves on quoted phrases).
std::string clean_search_query(const std::string& q);

}  // namespace ag
