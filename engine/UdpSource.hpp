#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../proto/messages.hpp"
#include "../proto/udp_framing.hpp"
#include "../sim/OutputSink.hpp"

// Live-feed counterpart to SessionReader. Deliberately exposes the SAME two
// members - header() and next(OddsUpdate&) - so the engine loop can be
// templated over the source rather than taking an interface: the read path
// keeps zero virtual calls, which is the rule the simulator's OutputSink is
// explicitly allowed to break and the engine is not.
//
// The synthesised header has seed/markets/books from the announce. ticks and
// record_count stay zero: a stream has no length, and inventing one would make
// the live path look like a file it is not.
class UdpSource {
public:
    UdpSource(uint16_t port, long announce_timeout_ms, long idle_ms)
        : idle_ms_(idle_ms), buf_(kMaxDatagramBytes) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("cannot create UDP socket");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
        addr.sin_port = ::htons(port);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("cannot bind UDP port");
        }
        int bufsz = 1 << 22;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));

        set_timeout(announce_timeout_ms);
        await_announce();
        set_timeout(idle_ms_);
    }

    ~UdpSource() { if (fd_ >= 0) ::close(fd_); }
    UdpSource(const UdpSource&) = delete;
    UdpSource& operator=(const UdpSource&) = delete;

    const SessionHeader& header() const { return header_; }

    bool next(OddsUpdate& out) {
        for (;;) {
            if (cursor_ < in_buffer_) {
                std::memcpy(&out,
                            buf_.data() + sizeof(UdpDatagramHeader) + cursor_ * sizeof(OddsUpdate),
                            sizeof(out));
                ++cursor_;
                ++records_;
                return true;
            }
            if (!receive_one()) return false;
        }
    }

    uint64_t datagrams() const { return datagrams_; }
    uint64_t records() const { return records_; }
    uint64_t lost() const { return lost_; }
    uint64_t reordered() const { return reordered_; }
    uint64_t malformed() const { return malformed_; }
    uint64_t pre_announce_discarded() const { return pre_announce_; }
    uint64_t announces() const { return announces_; }

private:
    void set_timeout(long ms) {
        timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    // Returns the datagram type, or -1 if the datagram was unusable.
    int read_datagram() {
        const ssize_t n = ::recv(fd_, buf_.data(), buf_.size(), 0);
        if (n < 0) return -2;  // timeout or error; caller decides
        if (static_cast<size_t>(n) < sizeof(UdpDatagramHeader)) { ++malformed_; return -1; }

        UdpDatagramHeader h{};
        std::memcpy(&h, buf_.data(), sizeof(h));
        if (h.magic != kUdpMagic || h.version != kUdpVersion) { ++malformed_; return -1; }
        if (h.record_count == 0) { ++malformed_; return -1; }

        const bool announce = (h.type == static_cast<uint8_t>(UdpDatagramType::Announce));
        const size_t want = announce
            ? sizeof(h) + sizeof(AnnounceRecord)
            : sizeof(h) + static_cast<size_t>(h.record_count) * sizeof(OddsUpdate);
        if (!announce && h.record_count > kRecordsPerDatagram) { ++malformed_; return -1; }
        if (static_cast<size_t>(n) != want) { ++malformed_; return -1; }

        // Sequence accounting spans both types, so a lost announce is counted
        // like any other loss rather than vanishing.
        if (h.datagram_seq > expected_seq_) {
            lost_ += h.datagram_seq - expected_seq_;
            expected_seq_ = h.datagram_seq + 1;
        } else if (h.datagram_seq < expected_seq_) {
            ++reordered_;
        } else {
            ++expected_seq_;
        }
        ++datagrams_;
        last_count_ = h.record_count;
        return announce ? 1 : 0;
    }

    void read_announce_payload(AnnounceRecord& a) const {
        std::memcpy(&a, buf_.data() + sizeof(UdpDatagramHeader), sizeof(a));
    }

    void await_announce() {
        for (;;) {
            const int t = read_datagram();
            if (t == -2) throw std::runtime_error(
                "timed out waiting for an announce datagram (is the simulator broadcasting?)");
            if (t == -1) continue;
            if (t == 0) { ++pre_announce_; continue; }  // data before we know the shape

            AnnounceRecord a{};
            read_announce_payload(a);
            if (a.record_size != sizeof(OddsUpdate))
                throw std::runtime_error("announce record size mismatch: sender and engine disagree");
            if (a.markets == 0 || a.books == 0)
                throw std::runtime_error("announce declares zero markets or books");

            header_ = SessionHeader{};
            header_.magic       = kSessionMagic;
            header_.version     = kSessionVersion;
            header_.record_size = static_cast<uint16_t>(sizeof(OddsUpdate));
            header_.seed        = a.seed;
            header_.markets     = a.markets;
            header_.books       = a.books;
            ++announces_;
            // Reset the sequence baseline: datagrams before the first announce
            // were discarded, and counting them as loss would misreport the
            // feed's quality for reasons that are our own late start.
            lost_ = 0;
            reordered_ = 0;
            in_buffer_ = 0;
            cursor_ = 0;
            return;
        }
    }

    bool receive_one() {
        for (;;) {
            const int t = read_datagram();
            if (t == -2) return false;   // idle timeout: treat as end of stream
            if (t == -1) continue;
            if (t == 1) {
                // Mid-stream announce. Config changing underneath a running
                // engine is not something to paper over silently.
                AnnounceRecord a{};
                read_announce_payload(a);
                if (a.markets != header_.markets || a.books != header_.books ||
                    a.seed != header_.seed)
                    throw std::runtime_error("announce changed mid-stream: feed reconfigured");
                ++announces_;
                continue;
            }
            in_buffer_ = last_count_;
            cursor_ = 0;
            return true;
        }
    }

    int fd_ = -1;
    long idle_ms_ = 0;
    SessionHeader header_{};
    std::vector<uint8_t> buf_;
    size_t in_buffer_ = 0;
    size_t cursor_ = 0;
    uint8_t last_count_ = 0;
    uint64_t expected_seq_ = 0;
    uint64_t datagrams_ = 0, records_ = 0;
    uint64_t lost_ = 0, reordered_ = 0, malformed_ = 0;
    uint64_t pre_announce_ = 0, announces_ = 0;
};
