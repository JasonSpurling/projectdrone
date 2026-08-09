// http_server.hpp -- minimal HTTP/1.1 server over raw POSIX sockets.
//
// Deliberately does NOT depend on libmicrohttpd or any other HTTP library.
// Those libraries' exact APIs vary across versions in ways that are risky
// to get right without being able to compile-test against the real headers
// (which this environment can't fetch). Plain sockets + a hand-rolled
// request-line/header parser is more code, but it's well-understood,
// version-independent, and only needs libc + pthreads -- both guaranteed
// present in any Docker base image.
#pragma once
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <regex>
#include <algorithm>
#include <thread>
#include <sstream>
#include <fstream>
#include <iostream>
#include <atomic>
#include <cctype>

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers; // keys lowercased
    std::string body;
    std::vector<std::string> path_params;

    // Case-insensitive by construction: header names were lowercased at
    // parse time, so callers don't need to know/guess the wire casing.
    std::string header(const std::string& name) const {
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = headers.find(lower);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
    std::map<std::string, std::string> extra_headers; // e.g. Content-Disposition for downloads

    static HttpResponse json_ok(const std::string& body_str) { return {200, body_str, "application/json"}; }
    static HttpResponse not_found() { return {404, R"({"error":"not found"})", "application/json"}; }
    static HttpResponse bad_request(const std::string& msg) {
        return {400, "{\"error\":\"" + msg + "\"}", "application/json"};
    }
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

inline std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int val = std::stoi(s.substr(i + 1, 2), nullptr, 16);
            out += static_cast<char>(val);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

class HttpServer {
public:
    struct Route { std::string method; std::regex path_regex; RouteHandler handler; };

    void get(const std::string& pattern, RouteHandler h) { routes_.push_back({"GET", std::regex(pattern), h}); }
    void post(const std::string& pattern, RouteHandler h) { routes_.push_back({"POST", std::regex(pattern), h}); }
    void serve_static_dir(const std::string& url_prefix, const std::string& dir) {
        static_prefix_ = url_prefix; static_dir_ = dir;
    }

    bool start(int port) {
        // Prefer a dual-stack IPv6 socket (IPV6_V6ONLY off) so both
        // "localhost" resolving to ::1 and plain IPv4 clients work --
        // an IPv4-only socket here means every OS/browser that tries ::1
        // first (Windows, and increasingly Linux/macOS) gets a silently
        // refused connection. Falls back to IPv4-only if the platform
        // doesn't support dual-stack sockets.
        server_fd_ = socket(AF_INET6, SOCK_STREAM, 0);
        if (server_fd_ >= 0) {
            int v6only = 0;
            setsockopt(server_fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            int opt = 1;
            setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            sockaddr_in6 addr6{};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr = in6addr_any;
            addr6.sin6_port = htons(port);
            if (bind(server_fd_, (sockaddr*)&addr6, sizeof(addr6)) == 0 &&
                listen(server_fd_, 128) == 0) {
                running_ = true;
                accept_thread_ = std::thread([this] { accept_loop(); });
                accept_thread_.detach();
                return true;
            }
            close(server_fd_);
            server_fd_ = -1;
        }

        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) return false;
        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) return false;
        if (listen(server_fd_, 128) < 0) return false;

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        accept_thread_.detach();
        return true;
    }

    void stop() { running_ = false; if (server_fd_ >= 0) close(server_fd_); }

private:
    static constexpr int MAX_CONCURRENT_CONNECTIONS = 200;
    // Without this, a client that connects and then sends nothing (or
    // trickles bytes in one at a time) parks a thread in recv() forever --
    // enough slow connections and every slot in MAX_CONCURRENT_CONNECTIONS
    // is permanently occupied by attackers, locking out real clients.
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 15;

    void accept_loop() {
        while (running_) {
            // sockaddr_storage, not sockaddr_in: server_fd_ may now be an
            // IPv6 socket, whose addresses don't fit in a sockaddr_in and
            // would overflow this buffer.
            sockaddr_storage client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &len);
            if (client_fd < 0) { if (running_) continue; else break; }

            // Cap concurrent connections -- otherwise a connection flood
            // spawns an unbounded number of OS threads and exhausts memory
            // even though single malformed requests can no longer crash
            // the process.
            if (active_connections_.load() >= MAX_CONCURRENT_CONNECTIONS) {
                close(client_fd);
                continue;
            }
            active_connections_.fetch_add(1);

            // Without this, Nagle's algorithm on the response write collides
            // with the client's delayed ACKs and throttles every response
            // (regardless of size) to a few hundred KB/s.
            int nodelay = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
            timeval tv{CONNECTION_TIMEOUT_SECONDS, 0};
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::thread(&HttpServer::handle_connection, this, client_fd).detach();
        }
    }

    void handle_connection(int fd) {
        // Guarantees active_connections_ is released on every exit path
        // (normal return or the catch-all below), so a slot is never
        // permanently leaked.
        struct ConnSlotGuard {
            std::atomic<int>& counter;
            ~ConnSlotGuard() { counter.fetch_sub(1); }
        } slot_guard{active_connections_};

        // This runs as a detached thread's entry point -- an exception
        // escaping here (e.g. from malformed input parsed below) has no
        // caller to propagate to, so the runtime calls std::terminate()
        // and takes down the *entire* server process, not just this
        // connection. A single crafted request from any unauthenticated
        // client was enough to kill the whole service; this catch-all is
        // the last line of defense against that class of bug regardless
        // of where in request handling it originates.
        try {
            std::string raw = read_request(fd);
            if (raw.empty()) { close(fd); return; }

            HttpRequest req;
            parse_request(raw, req);

            HttpResponse resp = route(req);
            std::string out = build_response(resp);
            send(fd, out.data(), out.size(), 0);
        } catch (...) {
            HttpResponse resp = HttpResponse::bad_request("malformed request");
            std::string out = build_response(resp);
            send(fd, out.data(), out.size(), 0);
        }
        close(fd);
    }

    static std::string read_request(int fd) {
        std::string data;
        char buf[8192];
        // Read headers first
        ssize_t n;
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return data;
            data.append(buf, n);
            header_end = data.find("\r\n\r\n");
            if (data.size() > 1024 * 1024) return data; // safety cap on header size
        }
        // Determine Content-Length and read remaining body if any.
        // A client can put an arbitrarily long digit string here (e.g.
        // 30+ nines) -- std::stoul throws std::out_of_range on that
        // rather than clamping, so this must not call it unguarded.
        size_t content_length = 0;
        std::smatch m;
        if (std::regex_search(data, m, std::regex("Content-Length:\\s*(\\d+)", std::regex::icase))) {
            try { content_length = std::stoul(m[1].str()); }
            catch (const std::exception&) { content_length = 0; }
        }
        content_length = std::min(content_length, static_cast<size_t>(64) * 1024 * 1024); // body size cap
        size_t body_have = data.size() - (header_end + 4);
        while (body_have < content_length) {
            n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, n);
            body_have += n;
        }
        return data;
    }

    static void parse_request(const std::string& raw, HttpRequest& req) {
        size_t line_end = raw.find("\r\n");
        std::string request_line = raw.substr(0, line_end);
        std::istringstream rl(request_line);
        std::string full_path;
        rl >> req.method >> full_path;

        size_t q = full_path.find('?');
        std::string path = (q == std::string::npos) ? full_path : full_path.substr(0, q);
        req.path = url_decode(path);
        if (q != std::string::npos) {
            std::string qs = full_path.substr(q + 1);
            std::istringstream qss(qs);
            std::string pair;
            while (std::getline(qss, pair, '&')) {
                size_t eq = pair.find('=');
                if (eq != std::string::npos) {
                    req.query[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
                }
            }
        }

        size_t header_end = raw.find("\r\n\r\n");
        size_t headers_start = (line_end == std::string::npos) ? raw.size() : line_end + 2;
        size_t headers_stop = (header_end == std::string::npos) ? raw.size() : header_end;
        if (headers_start < headers_stop) {
            std::istringstream hs(raw.substr(headers_start, headers_stop - headers_start));
            std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::string value = line.substr(colon + 1);
                size_t vstart = value.find_first_not_of(' ');
                req.headers[name] = (vstart == std::string::npos) ? "" : value.substr(vstart);
            }
        }

        if (header_end != std::string::npos) req.body = raw.substr(header_end + 4);
    }

    HttpResponse route(const HttpRequest& req_in) {
        HttpRequest req = req_in;
        for (auto& r : routes_) {
            if (r.method != req.method) continue;
            std::smatch m;
            if (std::regex_match(req.path, m, r.path_regex)) {
                for (size_t i = 1; i < m.size(); ++i) req.path_params.push_back(m[i].str());
                try { return r.handler(req); }
                catch (const std::exception& e) {
                    return {500, std::string("{\"error\":\"") + e.what() + "\"}", "application/json"};
                }
            }
        }
        if (!static_dir_.empty() && req.path.rfind(static_prefix_, 0) == 0) {
            std::string rel = req.path.substr(static_prefix_.size());
            // req.path is client-controlled and already URL-decoded, so a
            // request like /static/../../../etc/passwd (or .../proc/self/environ,
            // which would leak OPENSKY_CLIENT_SECRET straight out of the
            // process env) reaches here as "../../../etc/passwd" -- reject
            // any ".." path segment rather than concatenating it straight
            // into a filesystem path.
            if (!is_safe_relative_path(rel)) return HttpResponse::not_found();
            return serve_file(static_dir_ + "/" + (rel.empty() ? "index.html" : rel));
        }
        if (req.path == "/") return serve_file(static_dir_ + "/index.html");
        return HttpResponse::not_found();
    }

    // Rejects any path containing a ".." segment (checked per-segment, not
    // as a raw substring, so legitimate filenames like "my..file.png"
    // aren't blocked).
    static bool is_safe_relative_path(const std::string& rel) {
        size_t start = 0;
        while (start <= rel.size()) {
            size_t slash = rel.find('/', start);
            size_t seg_len = (slash == std::string::npos) ? rel.size() - start : slash - start;
            if (rel.compare(start, seg_len, "..") == 0) return false;
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return true;
    }

    static HttpResponse serve_file(const std::string& filepath) {
        std::ifstream f(filepath, std::ios::binary);
        if (!f) return HttpResponse::not_found();
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string ct = "text/plain";
        auto ends_with = [&](const char* suf) {
            std::string s(suf);
            return filepath.size() >= s.size() && filepath.compare(filepath.size() - s.size(), s.size(), s) == 0;
        };
        if (ends_with(".html")) ct = "text/html; charset=utf-8";
        else if (ends_with(".js")) ct = "application/javascript";
        else if (ends_with(".css")) ct = "text/css";
        else if (ends_with(".png")) ct = "image/png";
        else if (ends_with(".json")) ct = "application/json";
        return {200, ss.str(), ct};
    }

    static std::string build_response(const HttpResponse& resp) {
        std::ostringstream out;
        const char* status_text = resp.status == 200 ? "OK" : resp.status == 201 ? "Created"
            : resp.status == 400 ? "Bad Request" : resp.status == 401 ? "Unauthorized"
            : resp.status == 404 ? "Not Found" : resp.status == 500 ? "Internal Server Error" : "Unknown";
        out << "HTTP/1.1 " << resp.status << " " << status_text << "\r\n";
        out << "Content-Type: " << resp.content_type << "\r\n";
        out << "Content-Length: " << resp.body.size() << "\r\n";
        for (auto& [k, v] : resp.extra_headers) out << k << ": " << v << "\r\n";
        // No Access-Control-Allow-Origin here: the frontend is served from
        // this same server (same-origin fetches don't need it), and a
        // wildcard would let any website's JS cross-origin-read this API's
        // data -- track/incident locations, sensor status -- from any
        // browser on the network that happens to load that page.
        out << "Connection: close\r\n";
        out << "\r\n";
        out << resp.body;
        return out.str();
    }

    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> active_connections_{0};
    std::thread accept_thread_;
    std::vector<Route> routes_;
    std::string static_prefix_, static_dir_;
};
