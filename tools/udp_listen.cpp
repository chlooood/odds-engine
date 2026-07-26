#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../proto/messages.hpp"
#include "../proto/udp_framing.hpp"
#include "../sim/OutputSink.hpp"

// Receives an odds_sim --udp broadcast and optionally rebuilds a session file
// from it, so the captured stream can be checked with the existing
// verify_session tool.
//
// The capture is NOT byte-identical to the sender's session file: seed and
// tick count are not on the wire, so those header fields are left zero and
// markets/books are patched from what was actually observed. Compare record
// payloads, not whole files.

int main(int argc, char** argv) {
    uint16_t port = 0;
    std::string out_path;
    long idle_ms = 3000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", n); return nullptr; }
            return argv[++i];
        };
        if (a == "--port")          { auto v = next("--port");    if (!v) return 2; port = static_cast<uint16_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--out")      { auto v = next("--out");     if (!v) return 2; out_path = v; }
        else if (a == "--idle-ms")  { auto v = next("--idle-ms"); if (!v) return 2; idle_ms = std::strtol(v, nullptr, 10); }
        else if (a == "--help") {
            std::printf("usage: udp_listen --port N [--out PATH] [--idle-ms N]\n"
                        "  captures an odds_sim --udp stream; exits after --idle-ms of silence\n"
                        "  following the first datagram. Reports datagram loss and reordering.\n");
            return 0;
        } else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return 2; }
    }
    if (port == 0) { std::fprintf(stderr, "--port is required\n"); return 2; }

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { std::fprintf(stderr, "cannot create socket\n"); return 1; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    addr.sin_port = ::htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "cannot bind port %u\n", port);
        ::close(fd);
        return 1;
    }
    int bufsz = 1 << 22;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

    timeval tv{};
    tv.tv_sec  = idle_ms / 1000;
    tv.tv_usec = (idle_ms % 1000) * 1000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::FILE* out = nullptr;
    if (!out_path.empty()) {
        out = std::fopen(out_path.c_str(), "wb");
        if (!out) { std::fprintf(stderr, "cannot open %s\n", out_path.c_str()); ::close(fd); return 1; }
        SessionHeader h{};
        h.magic       = kSessionMagic;
        h.version     = kSessionVersion;
        h.record_size = static_cast<uint16_t>(sizeof(OddsUpdate));
        if (std::fwrite(&h, sizeof(h), 1, out) != 1) {
            std::fprintf(stderr, "failed writing capture header\n");
            std::fclose(out); ::close(fd); return 1;
        }
    }

    std::vector<uint8_t> buf(kMaxDatagramBytes);
    uint64_t datagrams = 0, records = 0, expected_seq = 0;
    uint64_t gaps = 0, reordered = 0, malformed = 0, announces = 0;
    uint32_t max_market = 0, max_book = 0;
    uint64_t ann_seed = 0; uint32_t ann_markets = 0, ann_books = 0;
    bool have_announce = false;

    std::printf("listening on port %u (exits after %ld ms of silence)\n", port, idle_ms);
    std::fflush(stdout);

    for (;;) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Idle timeout. Keep waiting if nothing has arrived yet, so the
                // listener can be started before the simulator.
                if (datagrams == 0) continue;
                break;
            }
            std::fprintf(stderr, "recv error\n");
            break;
        }
        if (static_cast<size_t>(n) < sizeof(UdpDatagramHeader)) { ++malformed; continue; }

        UdpDatagramHeader h{};
        std::memcpy(&h, buf.data(), sizeof(h));
        if (h.magic != kUdpMagic || h.version != kUdpVersion) { ++malformed; continue; }
        if (h.record_count == 0) { ++malformed; continue; }
        const bool is_announce = (h.type == static_cast<uint8_t>(UdpDatagramType::Announce));
        if (!is_announce && h.record_count > kRecordsPerDatagram) { ++malformed; continue; }
        const size_t want = is_announce
            ? sizeof(h) + sizeof(AnnounceRecord)
            : sizeof(h) + static_cast<size_t>(h.record_count) * sizeof(OddsUpdate);
        if (static_cast<size_t>(n) != want) { ++malformed; continue; }

        if (h.datagram_seq > expected_seq)      { gaps += h.datagram_seq - expected_seq; expected_seq = h.datagram_seq + 1; }
        else if (h.datagram_seq < expected_seq) { ++reordered; }
        else                                    { ++expected_seq; }

        if (is_announce) {
            AnnounceRecord a{};
            std::memcpy(&a, buf.data() + sizeof(h), sizeof(a));
            ann_seed = a.seed; ann_markets = a.markets; ann_books = a.books;
            have_announce = true;
            ++announces;
            ++datagrams;
            continue;
        }

        for (uint16_t r = 0; r < h.record_count; ++r) {
            OddsUpdate u{};
            std::memcpy(&u, buf.data() + sizeof(h) + r * sizeof(OddsUpdate), sizeof(u));
            if (u.market_id > max_market) max_market = u.market_id;
            if (u.book_id   > max_book)   max_book   = u.book_id;
            if (out && std::fwrite(&u, sizeof(u), 1, out) != 1) {
                std::fprintf(stderr, "short write to capture file\n");
                std::fclose(out); ::close(fd); return 1;
            }
            ++records;
        }
        ++datagrams;
    }
    ::close(fd);

    if (out) {
        // Patch what the wire actually told us. markets/books/seed now come
        // from the announce when one was seen, falling back to observation
        // otherwise. ticks stays zero: a stream has no length, and inventing
        // one would make the capture look more authoritative than it is.
        const uint32_t markets = have_announce ? ann_markets : max_market + 1;
        const uint32_t books   = have_announce ? ann_books   : max_book + 1;
        if (have_announce) {
            std::fseek(out, static_cast<long>(offsetof(SessionHeader, seed)), SEEK_SET);
            std::fwrite(&ann_seed, sizeof(ann_seed), 1, out);
        }
        std::fseek(out, static_cast<long>(offsetof(SessionHeader, markets)), SEEK_SET);
        std::fwrite(&markets, sizeof(markets), 1, out);
        std::fseek(out, static_cast<long>(offsetof(SessionHeader, books)), SEEK_SET);
        std::fwrite(&books, sizeof(books), 1, out);
        std::fseek(out, static_cast<long>(offsetof(SessionHeader, record_count)), SEEK_SET);
        std::fwrite(&records, sizeof(records), 1, out);
        std::fclose(out);
    }

    std::printf("received %llu datagrams (%llu announces), %llu records\n",
                (unsigned long long)datagrams, (unsigned long long)announces,
                (unsigned long long)records);
    if (have_announce)
        std::printf("announce: seed=%llu markets=%u books=%u\n",
                    (unsigned long long)ann_seed, ann_markets, ann_books);
    std::printf("lost %llu datagrams, %llu reordered, %llu malformed\n",
                (unsigned long long)gaps, (unsigned long long)reordered,
                (unsigned long long)malformed);
    if (!out_path.empty())
        std::printf("wrote capture to %s (markets=%u books=%u; tick count not on the wire)\n",
                    out_path.c_str(),
                    have_announce ? ann_markets : max_market + 1,
                    have_announce ? ann_books   : max_book + 1);
    return 0;
}
