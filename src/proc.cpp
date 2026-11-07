#include "proc.h"
#include "util.h"
#include <tlhelp32.h>
#include <vector>
#include <map>
#include <set>

namespace ag {

void proc_init() {
    const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (DWORD id : ids) {
        HANDLE h = GetStdHandle(id);
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }
}

std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    size_t bs = 0;
    for (char c : a) {
        if (c == '\\') { bs++; continue; }
        if (c == '"') { out.append(bs * 2 + 1, '\\'); out += '"'; bs = 0; continue; }
        out.append(bs, '\\');
        bs = 0;
        out += c;
    }
    out.append(bs * 2, '\\');
    out += '"';
    return out;
}

std::wstring build_env_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::map<std::wstring, std::pair<std::wstring, std::wstring>> vars;  // UPPER name -> (name, value)
    std::vector<std::wstring> special;  // "=C:=C:\..." entries keep as is
    wchar_t* env = GetEnvironmentStringsW();
    if (env) {
        for (wchar_t* p = env; *p; p += wcslen(p) + 1) {
            std::wstring e(p);
            if (e[0] == L'=') { special.push_back(e); continue; }
            size_t eq = e.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring name = e.substr(0, eq), up = name;
            for (auto& c : up) c = (wchar_t)towupper(c);
            vars[up] = std::make_pair(name, e.substr(eq + 1));
        }
        FreeEnvironmentStringsW(env);
    }
    for (auto& o : overrides) {
        std::wstring name = widen(o.first), up = name;
        for (auto& c : up) c = (wchar_t)towupper(c);
        if (o.second.empty()) vars.erase(up);
        else vars[up] = std::make_pair(name, widen(o.second));
    }
    std::wstring block;
    for (auto& s : special) { block += s; block += L'\0'; }
    for (auto& v : vars) {  // std::map keeps them sorted, as Windows expects
        block += v.second.first + L"=" + v.second.second;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

void kill_tree_by_snapshot(unsigned long root_pid) {
    // Collect descendants, then terminate leaves-first (repeat a few rounds for late spawns).
    for (int round = 0; round < 3; round++) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        std::multimap<DWORD, DWORD> children;
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe)) {
            do { children.insert(std::make_pair(pe.th32ParentProcessID, pe.th32ProcessID)); } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        std::vector<DWORD> order;
        std::set<DWORD> seen;
        std::vector<DWORD> stack{(DWORD)root_pid};
        while (!stack.empty()) {
            DWORD p = stack.back();
            stack.pop_back();
            if (!seen.insert(p).second) continue;
            order.push_back(p);
            auto r = children.equal_range(p);
            for (auto it = r.first; it != r.second; ++it)
                if (it->second != p && it->second != GetCurrentProcessId()) stack.push_back(it->second);
        }
        int killed = 0;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, *it);
            if (h) { if (TerminateProcess(h, 130)) killed++; CloseHandle(h); }
        }
        if (killed == 0) return;
        Sleep(100);
    }
}

static bool is_cancelled(HANDLE ev) { return ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0; }

static long job_active_processes(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai;
    ZeroMemory(&ai, sizeof ai);
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof ai, nullptr)) return 0;
    return (long)ai.ActiveProcesses;
}

long bg_prune(BackgroundSet& bg) {
    long running = 0;
    for (size_t i = 0; i < bg.jobs.size();) {
        long n = job_active_processes(bg.jobs[i]);
        if (n == 0) {
            CloseHandle(bg.jobs[i]);
            bg.jobs.erase(bg.jobs.begin() + i);
        } else {
            running += n;
            i++;
        }
    }
    return running;
}

void bg_kill_all(BackgroundSet& bg) {
    for (HANDLE j : bg.jobs) {
        TerminateJobObject(j, 130);
        CloseHandle(j);  // KILL_ON_JOB_CLOSE would also do it
    }
    bg.jobs.clear();
    for (unsigned long pid : bg.orphan_roots) kill_tree_by_snapshot(pid);
    bg.orphan_roots.clear();
}

void proc_run(const ProcRequest& req, ProcResult& res) {
    res = ProcResult();
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    std::wstring outw = widen(req.output_file);
    // The child's stdout/stderr: one inheritable handle (shared file position, so output stays in order).
    HANDLE wr = CreateFileW(outw.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (wr == INVALID_HANDLE_VALUE) { res.error = "cannot create " + req.output_file + ": " + win_error(GetLastError()); return; }
    HANDLE rd = CreateFileW(outw.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rd == INVALID_HANDLE_VALUE) { res.error = "cannot read " + req.output_file + ": " + win_error(GetLastError()); CloseHandle(wr); return; }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        // Closing the last handle kills whatever is left: covers conversation end and an agent
        // that is killed abruptly (the OS closes the handle).
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        ZeroMemory(&li, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);

    std::wstring app = widen(req.app);
    std::wstring cmd = widen(req.cmdline);
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(0);
    std::wstring cwd = widen(req.cwd);
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    void* envp = req.env_block.empty() ? nullptr : (void*)req.env_block.c_str();

    BOOL ok = CreateProcessW(app.c_str(), cmdbuf.data(), nullptr, nullptr, TRUE, flags | CREATE_BREAKAWAY_FROM_JOB, envp,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED)  // we are in a job that forbids breakaway
        ok = CreateProcessW(app.c_str(), cmdbuf.data(), nullptr, nullptr, TRUE, flags, envp,
                            cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD cerr = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        res.error = "CreateProcess(" + req.app + "): " + win_error(cerr);
        CloseHandle(rd);
        if (job) CloseHandle(job);
        return;
    }
    res.started = true;
    res.pid = pi.dwProcessId;
    bool in_job = job && AssignProcessToJobObject(job, pi.hProcess);
    if (!in_job) {
        res.tree_kill_by_job = false;
        res.error = "AssignProcessToJobObject failed (" + win_error(GetLastError()) + "); using snapshot tree kill";
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    char buf[8192];
    auto drain = [&]() {  // read everything written so far (ReadFile returns 0 bytes at the current end)
        while (true) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, sizeof buf, &got, nullptr) || got == 0) return;
            if (req.on_output) req.on_output(buf, got);
        }
    };

    HANDLE waits[2] = {pi.hProcess, req.cancel_event};
    DWORD nwaits = req.cancel_event ? 2 : 1;
    while (true) {
        drain();
        DWORD w = WaitForMultipleObjects(nwaits, waits, FALSE, 30);
        if (w == WAIT_OBJECT_0) break;  // main process exited: the call is finished
        if (w == WAIT_OBJECT_0 + 1 || is_cancelled(req.cancel_event)) {
            res.cancelled = true;
            if (in_job) TerminateJobObject(job, 130);
            else kill_tree_by_snapshot(pi.dwProcessId);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    drain();  // whatever is in the file now; background children may keep appending later
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exit_code = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) {
        long left = 0;
        if (in_job && !res.cancelled) {
            // processes that are just exiting still count for a moment
            for (int i = 0; i < 5 && (left = job_active_processes(job)) > 0; i++) Sleep(20);
        }
        if (left > 0 && req.background) {
            res.background_left = left;
            req.background->jobs.push_back(job);  // kept (and killed) until the conversation ends
        } else {
            CloseHandle(job);  // nothing left (or cancelled): KILL_ON_JOB_CLOSE has nothing to do
        }
    }
    if (!in_job && !res.cancelled && req.background) req.background->orphan_roots.push_back(pi.dwProcessId);
}

}  // namespace ag
