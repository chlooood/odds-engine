#pragma once
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Minimal RFC 6455 server, enough to push fair prices to a browser.
//
// Single-threaded and non-blocking by design: service() is called from the
// engine's own loop, so the engine is never descheduled waiting on a socket.
// engine-main-loop's "no threads, correctness first" rule holds - a background
// thread would need the fair-price state shared and locked, which is a Stage 3
// conversation, not a prerequisite for showing numbers in a browser.
//
// POSIX only, same as UdpSource. No TLS: this binds loopback for a local
// dashboard, and pretending otherwise with a half-secure implementation would
// be worse than being clear about it.
namespace ws {

// MSG_NOSIGNAL matters more than it looks. Writing to a socket whose peer has
// gone raises SIGPIPE, whose default action terminates the process - so without
// this, closing a browser tab would kill the engine mid-run.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

inline void sha1(const std::string& in, uint8_t out[20]) {
    uint32_t h0=0x67452301u,h1=0xEFCDAB89u,h2=0x98BADCFEu,h3=0x10325476u,h4=0xC3D2E1F0u;
    std::vector<uint8_t> m(in.begin(), in.end());
    const uint64_t bits = static_cast<uint64_t>(in.size()) * 8u;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0x00);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>((bits >> (i*8)) & 0xFF));

    for (size_t c = 0; c < m.size(); c += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint32_t>(m[c+i*4])   << 24)
                 | (static_cast<uint32_t>(m[c+i*4+1]) << 16)
                 | (static_cast<uint32_t>(m[c+i*4+2]) <<  8)
                 |  static_cast<uint32_t>(m[c+i*4+3]);
        for (int i = 16; i < 80; ++i) {
            const uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a=h0,b=h1,cc=h2,d=h3,e=h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & cc) | ((~b) & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6u; }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = cc; cc = (b << 30) | (b >> 2); b = a; a = t;
        }
        h0+=a; h1+=b; h2+=cc; h3+=d; h4+=e;
    }
    const uint32_t hs[5] = {h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[i*4+j] = static_cast<uint8_t>((hs[i] >> (24 - j*8)) & 0xFF);
}

inline std::string base64(const uint8_t* d, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(d[i]) << 16;
        if (i+1 < n) v |= static_cast<uint32_t>(d[i+1]) << 8;
        if (i+2 < n) v |= static_cast<uint32_t>(d[i+2]);
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += (i+1 < n) ? T[(v >> 6) & 0x3F] : '=';
        out += (i+2 < n) ? T[v & 0x3F]        : '=';
    }
    return out;
}

// Server-to-client frames are never masked, per RFC 6455 section 5.1.
inline void encode_text(const std::string& payload, std::vector<uint8_t>& out) {
    out.clear();
    out.push_back(0x81); // FIN | opcode 0x1 (text)
    const size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(n & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((n >> (i*8)) & 0xFF));
    }
    out.insert(out.end(), payload.begin(), payload.end());
}

inline const char* kPage = R"HTML(<!doctype html>
<meta charset=utf-8><title>odds-engine</title>
<style>
 body{background:#12141a;color:#d8dee9;font:13px/1.5 ui-monospace,Menlo,monospace;margin:0;padding:24px}
 h1{font-size:15px;font-weight:600;margin:0 0 2px}
 .sub{color:#6b7280;margin-bottom:18px}
 #s{display:inline-block;padding:2px 8px;border-radius:3px;background:#3b1d1d;color:#f87171}
 #s.on{background:#16301f;color:#4ade80}
 table{border-collapse:collapse;width:100%;max-width:760px}
 th{text-align:left;color:#6b7280;font-weight:500;padding:6px 10px;border-bottom:1px solid #262a33}
 td{padding:5px 10px;border-bottom:1px solid #1b1e26}
 .n{text-align:right;font-variant-numeric:tabular-nums}
 .bar{height:4px;background:#2563eb;border-radius:2px;min-width:1px}
</style>
<h1>odds-engine &middot; consensus fair price</h1>
<div class=sub><span id=s>connecting</span> <span id=meta></span></div>
<table><thead><tr><th>market</th><th class=n>home</th><th class=n>draw</th>
<th class=n>away</th><th class=n>books</th><th style=width:180px></th></tr></thead>
<tbody id=b></tbody></table>
<script>
const s=document.getElementById('s'),meta=document.getElementById('meta'),b=document.getElementById('b');
const ws=new WebSocket('ws://'+location.host+'/');
ws.onopen=()=>{s.textContent='live';s.className='on'};
ws.onclose=()=>{s.textContent='closed';s.className=''};
ws.onmessage=e=>{
  const d=JSON.parse(e.data);
  meta.textContent='tick '+d.t+' \u00b7 '+d.m.length+' markets';
  b.innerHTML=d.m.map(m=>{
    const p=m.p,w=Math.round(p[0]*100);
    return '<tr><td>'+m.i+'</td>'+p.map(x=>'<td class=n>'+x.toFixed(4)+'</td>').join('')+
      '<td class=n>'+m.b+'</td><td><div class=bar style=width:'+w+'%></div></td></tr>';
  }).join('');
};
</script>)HTML";

class Server {
public:
    explicit Server(uint16_t port, size_t max_backlog_bytes = 1u << 20)
        : max_backlog_(max_backlog_bytes) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) throw std::runtime_error("ws: cannot create socket");
        int one = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        // Loopback only. There is no TLS and no auth here; binding INADDR_ANY
        // would put an unauthenticated feed on the network.
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        a.sin_port = ::htons(port);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(listen_fd_); listen_fd_ = -1;
            throw std::runtime_error("ws: cannot bind port");
        }
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_); listen_fd_ = -1;
            throw std::runtime_error("ws: cannot listen");
        }
        set_nonblocking(listen_fd_);
    }

    ~Server() {
        for (auto& c : clients_) if (c.fd >= 0) ::close(c.fd);
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Accept, read, and flush. Never blocks: poll timeout is 0 throughout, so
    // a tick costs at most one poll syscall regardless of client behaviour.
    void service() {
        accept_new();
        if (clients_.empty()) return;

        pfds_.clear();
        for (auto& c : clients_) {
            pollfd p{};
            p.fd = c.fd;
            p.events = POLLIN | (c.out.empty() ? 0 : POLLOUT);
            pfds_.push_back(p);
        }
        if (::poll(pfds_.data(), pfds_.size(), 0) <= 0) return;

        for (size_t i = 0; i < pfds_.size(); ++i) {
            auto& c = clients_[i];
            if (pfds_[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { c.dead = true; continue; }
            if (pfds_[i].revents & POLLIN)  on_readable(c);
            if (pfds_[i].revents & POLLOUT) flush(c);
        }
        reap();
    }

    void broadcast(const std::string& text) {
        if (clients_.empty()) return;
        encode_text(text, frame_);
        for (auto& c : clients_) {
            if (c.dead || !c.open) continue;
            // Backpressure: a slow client gets updates dropped, never the
            // engine blocked. Prices are a snapshot, so a dropped frame costs
            // freshness rather than correctness - a queue that grows without
            // bound would cost both, eventually.
            if (c.out.size() + frame_.size() > max_backlog_) { ++dropped_; continue; }
            c.out.insert(c.out.end(), frame_.begin(), frame_.end());
            flush(c);
        }
        reap();
    }

    size_t client_count() const {
        size_t n = 0;
        for (const auto& c : clients_) if (c.open && !c.dead) ++n;
        return n;
    }
    uint64_t dropped() const { return dropped_; }
    uint64_t served_pages() const { return pages_; }

private:
    struct Client {
        int fd = -1;
        bool open = false;   // handshake complete
        bool dead = false;
        bool serve_only = false;  // plain HTTP request, not a websocket
        std::string in;
        std::vector<uint8_t> out;
    };

    static void set_nonblocking(int fd) {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        (void)::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    void accept_new() {
        for (;;) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            set_nonblocking(fd);
            int one = 1;
            // Fair prices are small and latency-sensitive; Nagle would coalesce
            // them into batches for no benefit here.
            (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            Client c; c.fd = fd;
            clients_.push_back(std::move(c));
        }
    }

    void on_readable(Client& c) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
            if (n == 0) { c.dead = true; return; }
            if (n < 0) break;                       // drained
            if (!c.open) {
                c.in.append(buf, static_cast<size_t>(n));
                if (c.in.size() > 16384) { c.dead = true; return; }  // junk, not a request
                if (c.in.find("\r\n\r\n") != std::string::npos) { handshake(c); return; }
            } else {
                // Only the close opcode matters to us: the dashboard sends no
                // commands, and a browser that goes away is detected here.
                for (ssize_t i = 0; i < n; ++i)
                    if ((static_cast<uint8_t>(buf[i]) & 0x0F) == 0x08) { c.dead = true; return; }
            }
        }
    }

    void handshake(Client& c) {
        const std::string key = header_value(c.in, "sec-websocket-key");
        if (key.empty()) { serve_page(c); return; }

        // RFC 6455: SHA-1 of key + this fixed GUID, base64'd.
        uint8_t d[20];
        sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", d);
        const std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + base64(d, 20) + "\r\n\r\n";
        c.out.assign(resp.begin(), resp.end());
        c.in.clear();
        c.open = true;
        flush(c);
    }

    void serve_page(Client& c) {
        const std::string body = kPage;
        const std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        c.out.assign(resp.begin(), resp.end());
        c.in.clear();
        c.serve_only = true;
        ++pages_;
        flush(c);
    }

    static std::string header_value(const std::string& req, const std::string& name) {
        std::string lower;
        lower.reserve(req.size());
        for (char ch : req) lower += static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        const size_t k = lower.find(name + ":");
        if (k == std::string::npos) return {};
        size_t b = req.find(':', k) + 1;
        while (b < req.size() && (req[b] == ' ' || req[b] == '\t')) ++b;
        const size_t e = req.find("\r\n", b);
        if (e == std::string::npos) return {};
        return req.substr(b, e - b);
    }

    void flush(Client& c) {
        while (!c.out.empty()) {
            const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                // EAGAIN means the kernel buffer is full: keep the remainder
                // queued and retry on the next POLLOUT. Anything else is fatal
                // for this client.
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                c.dead = true;
                return;
            }
            c.out.erase(c.out.begin(), c.out.begin() + n);
        }
        if (c.serve_only) c.dead = true;   // plain HTTP: one response, then close
    }

    void reap() {
        for (auto& c : clients_)
            if (c.dead && c.fd >= 0) { ::close(c.fd); c.fd = -1; }
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                           [](const Client& c) { return c.dead; }),
            clients_.end());
    }

    int listen_fd_ = -1;
    size_t max_backlog_;
    std::vector<Client> clients_;
    std::vector<pollfd> pfds_;
    std::vector<uint8_t> frame_;
    uint64_t dropped_ = 0;
    uint64_t pages_ = 0;
};

} // namespace ws
