#include <winsock2.h>
#include "http.h"
#include "util.h"
#include <cstring>
#include <cstdlib>

namespace ag {

bool winsock_init() {
    static bool done = false, ok = false;
    if (!done) {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
        done = true;
    }
    return ok;
}

namespace {

bool parse_url(const std::string& url, std::string& host, int& port, std::string& path, std::string& err) {
    std::string u = url;
    if (starts_with(lower(u), "http://")) u = u.substr(7);
    else if (starts_with(lower(u), "https://")) { err = "https is not supported (plain HTTP only)"; return false; }
    size_t slash = u.find('/');
    std::string hostport = slash == std::string::npos ? u : u.substr(0, slash);
    path = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hostport.rfind(':');
    port = 80;
    if (colon != std::string::npos) {
        port = atoi(hostport.c_str() + colon + 1);
        host = hostport.substr(0, colon);
    } else host = hostport;
    if (host.empty() || port <= 0 || port > 65535) { err = "bad URL: " + url; return false; }
    return true;
}

bool cancelled(HANDLE ev) { return ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0; }

std::string sock_err(int code) { return "socket " + win_error((DWORD)code); }

// Body framing decoder.
struct BodyDecoder {
    enum Mode { Length, Chunked, UntilClose } mode = UntilClose;
    long long remaining = 0;   // Length: bytes left; Chunked: bytes left in current chunk
    int cstate = 0;            // 0 = size line, 1 = data, 2 = CRLF after data, 3 = trailers, 4 = done
    std::string line;
    bool done = false;

    // returns false if the sink aborted
    template <class Sink>
    bool feed(const char* p, size_t n, Sink& sink) {
        if (mode == UntilClose) return sink(p, n);
        if (mode == Length) {
            size_t take = (size_t)((long long)n < remaining ? (long long)n : remaining);
            remaining -= take;
            if (remaining == 0) done = true;
            return take ? sink(p, take) : true;
        }
        size_t i = 0;
        while (i < n && !done) {
            if (cstate == 0 || cstate == 2 || cstate == 3) {
                char c = p[i++];
                if (c != '\n') { line += c; continue; }
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (cstate == 0) {
                    remaining = strtoll(line.c_str(), nullptr, 16);
                    cstate = remaining == 0 ? 3 : 1;
                } else if (cstate == 2) {
                    cstate = 0;
                } else {  // trailers until blank line
                    if (line.empty()) { done = true; cstate = 4; }
                }
                line.clear();
            } else if (cstate == 1) {
                size_t take = (size_t)((long long)(n - i) < remaining ? (long long)(n - i) : remaining);
                if (!sink(p + i, take)) return false;
                i += take;
                remaining -= take;
                if (remaining == 0) cstate = 2;
            }
        }
        return true;
    }
};

}  // namespace

void http_request(const std::string& method, const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                  const HttpOptions& opt, const std::function<bool(const char*, size_t)>& on_body, HttpResult& res) {
    res = HttpResult();
    std::string host, path, err;
    int port = 80;
    if (!parse_url(url, host, port, path, err)) { res.error = err; res.connect_failed = true; return; }
    if (!winsock_init()) { res.error = "WSAStartup failed"; res.connect_failed = true; return; }

    unsigned long addr = inet_addr(host.c_str());
    if (addr == INADDR_NONE) {
        hostent* he = gethostbyname(host.c_str());
        if (!he || !he->h_addr_list[0]) { res.error = "cannot resolve host " + host; res.connect_failed = true; return; }
        memcpy(&addr, he->h_addr_list[0], 4);
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { res.error = sock_err(WSAGetLastError()); res.connect_failed = true; return; }
    // never let children inherit the socket
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay);

    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = addr;

    struct Closer { SOCKET s; ~Closer() { closesocket(s); } } closer{s};

    if (connect(s, (sockaddr*)&sa, sizeof sa) != 0) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) { res.error = "connect: " + sock_err(e); res.connect_failed = true; return; }
        unsigned long start = tick_ms();
        while (true) {
            if (cancelled(opt.cancel_event)) { res.cancelled = true; res.error = "cancelled"; res.connect_failed = true; return; }
            fd_set wf, ef;
            FD_ZERO(&wf); FD_ZERO(&ef);
            FD_SET(s, &wf); FD_SET(s, &ef);
            timeval tv = {0, 200000};
            int r = select(0, nullptr, &wf, &ef, &tv);
            if (r > 0) {
                if (FD_ISSET(s, &ef)) {
                    int so = 0, len = sizeof so;
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &len);
                    res.error = "connect to " + host + ":" + std::to_string(port) + ": " + sock_err(so ? so : WSAECONNREFUSED);
                    res.connect_failed = true;
                    return;
                }
                break;
            }
            if (r < 0) { res.error = "select: " + sock_err(WSAGetLastError()); res.connect_failed = true; return; }
            if (tick_ms() - start > (unsigned long)opt.connect_timeout_ms) {
                res.error = "connect to " + host + ":" + std::to_string(port) + " timed out";
                res.timed_out = true;
                res.connect_failed = true;
                return;
            }
        }
    }

    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + (port != 80 ? ":" + std::to_string(port) : "") +
                      "\r\nConnection: close\r\nUser-Agent: agent/1.0\r\n";
    for (auto& h : headers) req += h.first + ": " + h.second + "\r\n";
    if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    req += body;

    size_t sent = 0;
    unsigned long last = tick_ms();
    while (sent < req.size()) {
        if (cancelled(opt.cancel_event)) { res.cancelled = true; res.error = "cancelled"; return; }
        int n = send(s, req.data() + sent, (int)((req.size() - sent) > 65536 ? 65536 : (req.size() - sent)), 0);
        if (n > 0) { sent += n; last = tick_ms(); continue; }
        int e = WSAGetLastError();
        if (n < 0 && e != WSAEWOULDBLOCK) { res.error = "send: " + sock_err(e); return; }
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        timeval tv = {0, 200000};
        select(0, nullptr, &wf, nullptr, &tv);
        if (tick_ms() - last > (unsigned long)opt.idle_timeout_ms) { res.error = "send timed out"; res.timed_out = true; return; }
    }

    // ---- receive
    std::string head;
    bool have_head = false;
    BodyDecoder dec;
    bool stream_to_cb = false;
    bool aborted = false;
    auto sink = [&](const char* p, size_t n) -> bool {
        if (stream_to_cb) {
            if (!on_body(p, n)) { aborted = true; return false; }
            return true;
        }
        if (res.body.size() + n > opt.max_collect) n = opt.max_collect - res.body.size();
        res.body.append(p, n);
        return true;
    };

    char buf[16384];
    last = tick_ms();
    while (true) {
        if (cancelled(opt.cancel_event)) { res.cancelled = true; res.error = "cancelled"; return; }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        timeval tv = {0, 200000};
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (r < 0) { res.error = "select: " + sock_err(WSAGetLastError()); return; }
        if (r == 0) {
            if (tick_ms() - last > (unsigned long)opt.idle_timeout_ms) {
                res.error = "no data for " + std::to_string(opt.idle_timeout_ms / 1000) + " s";
                res.timed_out = true;
                return;
            }
            continue;
        }
        int n = recv(s, buf, sizeof buf, 0);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) continue;
            res.error = "recv: " + sock_err(e);
            return;
        }
        if (n == 0) {  // connection closed
            if (!have_head) { res.error = "connection closed before response headers"; return; }
            if (dec.mode == BodyDecoder::UntilClose) res.complete = true;
            else if (!dec.done) res.error = "connection closed mid-body";
            return;
        }
        last = tick_ms();
        const char* p = buf;
        size_t len = (size_t)n;
        if (!have_head) {
            head.append(buf, n);
            size_t e = head.find("\r\n\r\n");
            if (e == std::string::npos) {
                if (head.size() > 65536) { res.error = "response headers too large"; return; }
                continue;
            }
            std::string rest = head.substr(e + 4);
            head.resize(e);
            // status line
            size_t sp = head.find(' ');
            res.status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
            if (res.status >= 100 && res.status < 200) {  // skip interim response
                head = rest;
                continue;
            }
            have_head = true;
            long long clen = -1;
            bool chunked = false;
            for (auto& l : split_lines(head)) {
                size_t c = l.find(':');
                if (c == std::string::npos) continue;
                std::string k = lower(trim(l.substr(0, c))), v = trim(l.substr(c + 1));
                if (k == "content-length") clen = atoll(v.c_str());
                else if (k == "transfer-encoding" && lower(v).find("chunked") != std::string::npos) chunked = true;
                else if (k == "content-type") res.content_type = v;
            }
            if (chunked) dec.mode = BodyDecoder::Chunked;
            else if (clen >= 0) { dec.mode = BodyDecoder::Length; dec.remaining = clen; if (clen == 0) dec.done = true; }
            stream_to_cb = on_body && res.status >= 200 && res.status < 300;
            head.swap(rest);
            p = head.data();
            len = head.size();
            if (len == 0) {
                if (dec.done) { res.complete = true; return; }
                continue;
            }
        }
        if (!dec.feed(p, len, sink)) {
            if (aborted) { res.cancelled = true; res.error = "aborted by caller"; }
            return;
        }
        if (dec.done) { res.complete = true; return; }
    }
}

// ---------------------------------------------------------------- SSE

bool SseParser::feed(const char* p, size_t n) {
    buf_.append(p, n);
    size_t start = 0;
    while (true) {
        size_t nl = buf_.find('\n', start);
        if (nl == std::string::npos) break;
        size_t e = nl;
        if (e > start && buf_[e - 1] == '\r') e--;
        if (!line(buf_.substr(start, e - start))) { buf_.erase(0, nl + 1); return false; }
        start = nl + 1;
    }
    buf_.erase(0, start);
    return true;
}

bool SseParser::line(const std::string& l) {
    if (l.empty()) {
        if (!have_data_) return true;
        std::string d;
        d.swap(data_);
        have_data_ = false;
        return cb_(d);
    }
    if (l[0] == ':') return true;  // comment / keep-alive
    if (starts_with(l, "data:")) {
        std::string v = l.substr(5);
        if (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (have_data_) data_ += '\n';
        data_ += v;
        have_data_ = true;
    }
    return true;  // event:, id:, retry: ignored
}

bool SseParser::finish() {
    if (!buf_.empty()) { std::string l = buf_; buf_.clear(); if (!line(l)) return false; }
    return line("");
}

}  // namespace ag
