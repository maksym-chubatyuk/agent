// agent backend: the public in-process API for a frontend.
//
// Threads: Agent::start() creates exactly ONE long-lived worker thread that runs turns. Every
// other call is made from the frontend's own thread(s). Nothing else ever creates a thread, which
// matters on the target XP kernel (thread stacks are never freed).
//
// Events are JSON objects with a "type" field (see NOTES.md for the full list). They are delivered
// either to the callback (called on the worker thread; keep it short and non-blocking) or, when no
// callback is set, to an internal queue drained with next_event().
#pragma once
#include "json.h"
#include "config.h"
#include "session.h"
#include <functional>
#include <string>
#include <vector>

namespace ag {

class AgentImpl;

class Agent {
public:
    Agent();
    ~Agent();  // calls shutdown()

    // Load config, data dir, persona/memory, approval patterns; start the worker thread.
    bool start(const Config& cfg, std::string* err);
    void shutdown();  // cancels a running turn, joins the worker, ends the conversation

    // ---- events
    void set_event_callback(std::function<void(const Json& ev)> cb);  // set before start()
    bool next_event(Json& ev, unsigned timeout_ms);  // queue mode; false on timeout

    // ---- commands (thread-safe)
    // Start a turn asynchronously. Fails if a turn is already running. Opens a new session first
    // if none is open. Emits turn_started ... turn_finished.
    bool start_turn(const std::string& text, std::string* err);
    void cancel();  // stop generation / kill a running bash tree / deny a pending approval
    // Answer an approval_request. Returns false if no such request is pending.
    bool answer_approval(const std::string& request_id, bool approve);
    std::string pending_approval();  // request_id of the approval being waited for, or ""

    // Session management: not allowed while a turn is running (returns false, err="busy").
    // A conversation = an open session + its scratchpad + its background processes.
    bool new_session(std::string* id, std::string* err);
    bool resume_session(const std::string& id, std::string* err);  // id or "last"
    std::vector<SessionInfo> list_sessions();
    // Remove the last turn from the conversation history (messages only; files are not touched).
    bool undo_last_turn(Json* report, std::string* err);  // report: {turn, messages}
    // End the conversation: kill its background processes, delete its scratchpad, close the session.
    // Also done by new_session, resume_session and shutdown.
    bool close_session(std::string* err);
    std::string scratch_dir();  // scratchpad of the open conversation ("" if none)
    Json history();  // live message array of the open session (copy)
    std::string session_id();

    bool busy();
    void set_yolo(bool on);
    bool yolo();
    const Config& config() const;

private:
    AgentImpl* impl_;
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
};

}  // namespace ag
