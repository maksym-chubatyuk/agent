// Child process runner. One Job object per command (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE).
// stdout+stderr go to a file that is polled from the calling thread (no helper threads, and no
// pipe EOF to wait for: a call is finished as soon as its main process exits). Jobs that still
// contain background processes are kept in a BackgroundSet until the conversation ends.
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <windows.h>

namespace ag {

// Background processes left behind by finished commands, owned by one conversation.
struct BackgroundSet {
    std::vector<HANDLE> jobs;                 // jobs with active processes (KILL_ON_JOB_CLOSE set)
    std::vector<unsigned long> orphan_roots;  // root pids whose tree could not be put in a job
};
// Close the jobs whose processes have all exited. Returns the number of processes still running.
long bg_prune(BackgroundSet& bg);
// Kill every remaining background process and close the jobs (conversation end).
void bg_kill_all(BackgroundSet& bg);

struct ProcRequest {
    std::string app;          // full path of the executable
    std::string cmdline;      // complete command line (first token = program)
    std::string cwd;          // working directory ("" = inherit)
    std::wstring env_block;   // double-NUL-terminated UTF-16 environment ("" = inherit)
    std::string output_file;  // where the child's stdout+stderr are written (created/truncated)
    HANDLE cancel_event = nullptr;
    BackgroundSet* background = nullptr;  // receives the job if processes outlive the main one
    std::function<void(const char*, size_t)> on_output;  // called with raw output bytes
};

struct ProcResult {
    bool started = false;
    bool cancelled = false;
    int exit_code = -1;
    std::string error;        // why it failed to start / anything odd
    bool tree_kill_by_job = true;  // false if the job could not be used and the snapshot fallback was used
    unsigned long pid = 0;
    long background_left = 0; // processes still running in this command's job after it finished
};

void proc_run(const ProcRequest& req, ProcResult& res);

// Make this process's std handles non-inheritable so children (and their background grandchildren)
// don't keep our caller's pipes open.
void proc_init();

// Build a UTF-16 environment block from the current environment with overrides (name -> value; "" removes).
std::wstring build_env_block(const std::vector<std::pair<std::string, std::string>>& overrides);

// Quote one argument for CreateProcess / MSVCRT argv parsing.
std::string quote_arg(const std::string& a);

// Kill every process whose ancestry leads to root_pid (fallback when Job objects are unavailable).
void kill_tree_by_snapshot(unsigned long root_pid);

}  // namespace ag
