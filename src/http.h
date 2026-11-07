// HTTP/1.1 client on WinSock2 (plain HTTP only). Non-blocking socket polled with select(), so a
// request runs entirely on the calling thread and can be cancelled through a Win32 event.
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <windows.h>

namespace ag {

struct HttpOptions {
    int connect_timeout_ms = 10000;
    int idle_timeout_ms = 120000;   // max time without receiving any byte
    HANDLE cancel_event = nullptr;  // manual-reset event; when signalled the request aborts
    size_t max_collect = 64 << 20;  // cap for the collected body
};

struct HttpResult {
    int status = 0;                 // HTTP status, 0 if no response
    bool complete = false;          // response fully received according to its framing
    bool cancelled = false;
    bool timed_out = false;
    bool connect_failed = false;    // never connected (refused / unreachable / resolve failure)
    std::string error;              // transport error text ("" if none)
    std::string body;               // collected body (when no streaming callback, or on non-2xx)
    std::string content_type;
};

// on_body: receives decoded body bytes as they arrive for 2xx responses; return false to abort.
// If on_body is empty the body is collected into result.body.
void http_request(const std::string& method, const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& headers, const std::string& body,
                  const HttpOptions& opt, const std::function<bool(const char*, size_t)>& on_body, HttpResult& res);

// Incremental Server-Sent-Events parser. Feed bytes; on_event is called with each event's data
// (multiple data: lines joined with \n).
class SseParser {
public:
    explicit SseParser(std::function<bool(const std::string& data)> on_event) : cb_(std::move(on_event)) {}
    bool feed(const char* p, size_t n);  // false if the callback asked to stop
    bool finish();                       // flush a pending event without trailing blank line
private:
    bool line(const std::string& l);
    std::function<bool(const std::string&)> cb_;
    std::string buf_, data_;
    bool have_data_ = false;
};

bool winsock_init();

}  // namespace ag
