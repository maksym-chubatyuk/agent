#include "util.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

namespace ag {

// ---------------------------------------------------------------- strings

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) {  // invalid UTF-8: fall back to ANSI
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool valid_utf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        int len;
        unsigned cp;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

std::string codepage_to_utf8(const std::string& s, unsigned cp) {
    if (s.empty()) return s;
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;
    std::wstring w(n, 0);
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return narrow(w);
}

std::string output_to_utf8(const std::string& bytes) {
    if (valid_utf8(bytes)) return bytes;
    return codepage_to_utf8(bytes, CP_OEMCP);
}

std::string utf8_head(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

std::string utf8_tail(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t start = s.size() - n;
    while (start < s.size() && ((unsigned char)s[start] & 0xC0) == 0x80) start++;
    return s.substr(start);
}

std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

size_t count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t c = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { c++; pos += needle.size(); }
    return c;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            if (start < s.size()) out.push_back(s.substr(start));
            break;
        }
        size_t e = nl;
        if (e > start && s[e - 1] == '\r') e--;
        out.push_back(s.substr(start, e - start));
        start = nl + 1;
    }
    return out;
}

// ---------------------------------------------------------------- files

bool read_file(const std::string& path, std::string& out) {
    HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[65536];
    DWORD got;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

static bool write_impl(const std::string& path, const std::string& data, bool append) {
    HANDLE h = CreateFileW(widen(path).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        DWORD put = 0;
        DWORD chunk = (DWORD)((data.size() - off) > (1u << 20) ? (1u << 20) : (data.size() - off));
        if (!WriteFile(h, data.data() + off, chunk, &put, nullptr) || put == 0) { ok = false; break; }
        off += put;
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::string& path, const std::string& data) { return write_impl(path, data, false); }
bool append_file(const std::string& path, const std::string& data) { return write_impl(path, data, true); }

bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::string& path) {
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& path) {
    if (path.empty() || dir_exists(path)) return true;
    std::string p = to_backslashes(path);
    while (!p.empty() && p.back() == '\\') p.pop_back();
    if (p.empty() || (p.size() == 2 && p[1] == ':')) return true;
    std::string parent = path_dir(p);
    if (!parent.empty() && parent != p && !make_dirs(parent)) return false;
    if (CreateDirectoryW(widen(p).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool delete_file(const std::string& path) { return DeleteFileW(widen(path).c_str()) != 0; }
bool copy_file(const std::string& from, const std::string& to) {
    return CopyFileW(widen(from).c_str(), widen(to).c_str(), FALSE) != 0;
}
long long file_size(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

std::vector<std::string> list_dir(const std::string& dir, const std::string& pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(path_join(dir, pattern)).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::vector<std::string> list_subdirs(const std::string& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(path_join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static void delete_tree_once(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                delete_tree_once(p);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());  // junction: unlink only
                else DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(dir.c_str());
}

bool delete_tree(const std::string& dir) {
    if (dir.empty()) return true;
    std::wstring w = widen(to_backslashes(dir));
    while (!w.empty() && w.back() == L'\\') w.pop_back();
    for (int attempt = 0; attempt < 30; attempt++) {
        delete_tree_once(w);
        if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
        Sleep(100);
    }
    return false;
}

// ---------------------------------------------------------------- paths

std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (path_is_absolute(b)) return b;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string path_dir(const std::string& p) {
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return "";
    if (pos == 2 && p.size() > 1 && p[1] == ':') return p.substr(0, 3);
    return p.substr(0, pos);
}

bool path_is_absolute(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return true;
    return false;
}

std::string to_backslashes(std::string p) {
    for (auto& c : p) if (c == '/') c = '\\';
    return p;
}
std::string to_slashes(std::string p) {
    for (auto& c : p) if (c == '\\') c = '/';
    return p;
}

std::string full_path(const std::string& p) {
    std::wstring w = widen(to_backslashes(p));
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH * 2, buf, nullptr);
    if (n == 0 || n >= MAX_PATH * 2) return p;
    return narrow(std::wstring(buf, n));
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return path_dir(narrow(std::wstring(buf, n)));
}

std::string msys_to_win(const std::string& p) {
    if (p.size() >= 2 && p[0] == '/' && isalpha((unsigned char)p[1]) && (p.size() == 2 || p[2] == '/')) {
        std::string r;
        r += (char)toupper((unsigned char)p[1]);
        r += ":/";
        if (p.size() > 3) r += p.substr(3);
        return r;
    }
    return p;
}

// ---------------------------------------------------------------- time

std::string now_iso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[40];
    snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
    return buf;
}

std::string now_stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[40];
    snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

unsigned long tick_ms() { return GetTickCount(); }

std::string win_error(DWORD code) {
    char buf[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf,
                             sizeof buf, nullptr);
    std::string s = n ? trim(std::string(buf, n)) : std::string();
    return "error " + std::to_string(code) + (s.empty() ? "" : " (" + codepage_to_utf8(s, CP_ACP) + ")");
}

// ---------------------------------------------------------------- error log

static Mutex g_log_mu;
static std::string g_log_path;
static char g_log_path_ansi[MAX_PATH * 2];

void set_error_log_path(const std::string& path) {
    Lock l(g_log_mu);
    g_log_path = path;
    std::wstring w = widen(path);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, g_log_path_ansi, sizeof g_log_path_ansi, nullptr, nullptr);
}

void log_error(const std::string& msg) {
    Lock l(g_log_mu);
    std::string line = now_iso() + " [pid " + std::to_string(GetCurrentProcessId()) + "] " + msg + "\r\n";
    std::string p = g_log_path.empty() ? path_join(exe_dir(), "agent-error.log") : g_log_path;
    append_file(p, line);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    // Keep this minimal: no heap, no locks.
    char buf[256];
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    HMODULE mod = GetModuleHandleW(nullptr);
    int n = snprintf(buf, sizeof buf, "CRASH: exception 0x%08lx at %p (exe base %p), thread %lu\r\n",
                     (unsigned long)code, addr, (void*)mod, (unsigned long)GetCurrentThreadId());
    const char* path = g_log_path_ansi[0] ? g_log_path_ansi : "agent-error.log";
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, buf, (DWORD)n, &w, nullptr);
        CloseHandle(h);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void install_crash_handler() { SetUnhandledExceptionFilter(crash_filter); }

// ---------------------------------------------------------------- stats

ProcStats proc_stats() {
    ProcStats s;
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid) s.threads++;
                te.dwSize = sizeof te;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
    MEMORY_BASIC_INFORMATION mbi;
    char* p = (char*)0x10000;
    while ((unsigned long)p < 0x7FFF0000UL && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned long kb = (unsigned long)(mbi.RegionSize / 1024);
        if (mbi.State == MEM_FREE) {
            if (kb > s.largest_free_kb) s.largest_free_kb = kb;
        } else {
            s.reserved_kb += kb;
            if (mbi.State == MEM_COMMIT) s.committed_kb += kb;
        }
        char* next = (char*)mbi.BaseAddress + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    return s;
}

}  // namespace ag
