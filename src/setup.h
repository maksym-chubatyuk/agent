// First-run setup: plain console prompts that write an agent.ini (see NOTES.md, "Setup").
#pragma once
#include "config.h"
#include <string>

namespace ag {

// Asks for the endpoint, API key, model, context length, Firecrawl URL and bash.exe, then writes
// `path` in agent.ini.template's format. `cur` (may be null) supplies the defaults and the values of
// the keys that are not asked. Returns false, with *err set, if it can't run or write the file.
bool run_setup(const Config* cur, const std::string& path, std::string* err);

}  // namespace ag
