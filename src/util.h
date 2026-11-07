// Small Win32 helpers: UTF-8 <-> UTF-16, files, paths, time, locking, error log.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace ag {

// ---- strings
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
bool valid_utf8(const std::string& s);
std::string codepage_to_utf8(const std::string& s, unsigned cp);
// Bytes from a child process: kept if valid UTF-8, else decoded from the OEM code page.
std::string output_to_utf8(const std::string& bytes);
// Cut at <= n bytes without splitting a UTF-8 sequence.
std::string utf8_head(const std::string& s, size_t n);
std::string utf8_tail(const std::string& s, size_t n);
std::string lower(std::string s);
std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
size_t count_occurrences(const std::string& hay, const std::string& needle);
std::vector<std::string> split_lines(const std::string& s);  // splits on \n, strips \r

// ---- files (UTF-8 paths)
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);
bool append_file(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
bool make_dirs(const std::string& path);
bool delete_file(const std::string& path);
bool copy_file(const std::string& from, const std::string& to);
long long file_size(const std::string& path);
std::vector<std::string> list_dir(const std::string& dir, const std::string& pattern);  // names only
std::vector<std::string> list_subdirs(const std::string& dir);  // names only, without . and ..
// Delete a directory tree (read-only files too). Retries for a while, because processes that were
// just killed may still hold files open. Returns true if the directory is gone.
bool delete_tree(const std::string& dir);

// ---- paths
std::string path_join(const std::string& a, const std::string& b);
std::string path_dir(const std::string& p);
bool path_is_absolute(const std::string& p);
std::string to_backslashes(std::string p);
std::string to_slashes(std::string p);
std::string full_path(const std::string& p);  // GetFullPathNameW
std::string exe_dir();
// "/c/foo" -> "C:/foo"; leaves everything else alone.
std::string msys_to_win(const std::string& p);

// ---- time
std::string now_iso();      // 2026-11-07T17:22:01Z (UTC)
std::string now_stamp();    // 20261107-172201 (local)
unsigned long tick_ms();    // GetTickCount

// ---- last Win32 error as text
std::string win_error(DWORD code);

// ---- locking
class Mutex {
public:
    Mutex() { InitializeCriticalSection(&cs_); }
    ~Mutex() { DeleteCriticalSection(&cs_); }
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
private:
    CRITICAL_SECTION cs_;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
};
class Lock {
public:
    explicit Lock(Mutex& m) : m_(m) { m_.lock(); }
    ~Lock() { m_.unlock(); }
private:
    Mutex& m_;
};

// ---- error log for failures outside any session
void set_error_log_path(const std::string& path);
void log_error(const std::string& msg);
void install_crash_handler();

// ---- process stats (for the thread-leak check)
struct ProcStats {
    int threads = 0;
    unsigned long reserved_kb = 0;   // MEM_RESERVE + MEM_COMMIT regions
    unsigned long committed_kb = 0;
    unsigned long largest_free_kb = 0;
};
ProcStats proc_stats();

}  // namespace ag
