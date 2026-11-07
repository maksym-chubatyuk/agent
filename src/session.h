// Append-only JSONL session store. The live history is rebuilt by replaying the file; undo and
// compression are records too, so nothing is ever rewritten in place. Undo affects messages only.
#pragma once
#include "json.h"
#include <string>
#include <vector>
#include <set>

namespace ag {

struct SessionInfo {
    std::string id;
    std::string path;
    std::string created;   // ISO time from the meta record
    std::string updated;   // file mtime, ISO
    std::string title;     // first user message (truncated)
    long long bytes = 0;
};

class Session {
public:
    std::string id;
    std::string path;
    Json messages = Json::array();  // live OpenAI-style history (no system prompt)
    int last_turn = 0;          // highest turn number ever used (undone turns are not reused)
    std::vector<int> live_turns;
    std::string cwd;            // last recorded working directory ("" = default)

    bool create(const std::string& sessions_dir, const Json& meta_extra, std::string* err);
    bool open(const std::string& sessions_dir, const std::string& id, std::string* err);
    bool is_open() const { return !path.empty(); }

    bool append(const Json& rec);  // writes one line (adds "ts")
    // helpers that append and apply to the in-memory state
    int begin_turn();
    void add_message(int turn, const Json& msg);
    void add_event(int turn, const Json& ev);
    void set_cwd(int turn, const std::string& cwd);
    void apply_compaction(int turn, size_t cut, const Json& summary_msg, const Json& info);

    // Undo the most recent live turn: append an undo record and replay. Only the conversation
    // history changes; files on disk are never touched.
    bool undo_last(Json& report, std::string* err);

    static std::vector<SessionInfo> list(const std::string& sessions_dir);
    static std::string latest_id(const std::string& sessions_dir);

private:
    bool replay(std::string* err);
};

}  // namespace ag
