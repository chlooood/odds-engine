#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../proto/messages.hpp"
#include "../proto/timing.hpp"
#include "../proto/udp_framing.hpp"
#include "OutputSink.hpp"

// Second OutputSink implementation. The generation loop is untouched: it still
// calls accept() on an OutputSink& and knows nothing about sockets.
//
// POSIX sockets only (Linux/WSL/macOS). A Windows build would need Winsock,
// which is not attempted here.
class UdpSink final : public OutputSink {
public:
    UdpSink(const std::string& host, uint16_t port,
            uint64_t seed, uint32_t markets, uint32_t books)
        : seed_(seed), markets_(markets), books_(books) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("cannot create UDP socket");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("bad --udp host address: " + host);
        }
        // connect() on a datagram socket fixes the peer, so the hot path can
        // use send() and skip re-resolving the destination per datagram.
        if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("cannot connect UDP socket");
        }
        // Best-effort: a larger send buffer reduces ENOBUFS bursts when the
        // generation loop outruns the kernel. Failure here is not fatal.
        int bufsz = 1 << 21;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

        // Announce before any data, so a receiver already listening can size
        // its table without waiting a full announce interval.
        send_announce();
    }

    ~UdpSink() override { if (fd_ >= 0) ::close(fd_); }
    UdpSink(const UdpSink&) = delete;
    UdpSink& operator=(const UdpSink&) = delete;

    void accept(const OddsUpdate& update) override {
        buffer_[fill_++] = update;
        if (fill_ == kRecordsPerDatagram) flush();
    }

    void finish() override {
        flush();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    uint64_t datagrams_sent() const { return datagram_seq_; }
    uint64_t announces_sent() const { return announces_; }
    uint64_t send_errors() const { return send_errors_; }

private:
    void send_packet(size_t len) {
        // A failed send is counted, not thrown. Losing a datagram is the normal
        // failure mode of this transport; aborting a long generation run over
        // one dropped packet would be the wrong trade.
        if (::send(fd_, packet_.data(), len, 0) != static_cast<ssize_t>(len)) ++send_errors_;
        ++datagram_seq_;
    }

    void send_announce() {
        UdpDatagramHeader h{};
        h.magic        = kUdpMagic;
        h.version      = kUdpVersion;
        h.type         = static_cast<uint8_t>(UdpDatagramType::Announce);
        h.record_count = 1;
        h.datagram_seq = datagram_seq_;

        AnnounceRecord a{};
        a.seed             = seed_;
        a.tick_interval_ns = kTickIntervalNs;
        a.markets          = markets_;
        a.books            = books_;
        a.record_size      = static_cast<uint32_t>(sizeof(OddsUpdate));

        std::memcpy(packet_.data(), &h, sizeof(h));
        std::memcpy(packet_.data() + sizeof(h), &a, sizeof(a));
        send_packet(sizeof(h) + sizeof(a));
        ++announces_;
        since_announce_ = 0;
    }

    void flush() {
        if (fill_ == 0) return;
        if (since_announce_ >= kAnnounceInterval) send_announce();

        UdpDatagramHeader h{};
        h.magic        = kUdpMagic;
        h.version      = kUdpVersion;
        h.type         = static_cast<uint8_t>(UdpDatagramType::Data);
        h.record_count = static_cast<uint8_t>(fill_);
        h.datagram_seq = datagram_seq_;

        const size_t len = sizeof(h) + fill_ * sizeof(OddsUpdate);
        std::memcpy(packet_.data(), &h, sizeof(h));
        std::memcpy(packet_.data() + sizeof(h), buffer_.data(), fill_ * sizeof(OddsUpdate));
        send_packet(len);
        ++since_announce_;
        fill_ = 0;
    }

    int fd_ = -1;
    uint64_t seed_ = 0;
    uint32_t markets_ = 0;
    uint32_t books_ = 0;
    // Both buffers are members, not locals: nothing allocates inside the
    // generation loop, same discipline as FileSink.
    std::array<OddsUpdate, kRecordsPerDatagram> buffer_{};
    std::array<uint8_t, kMaxDatagramBytes> packet_{};
    size_t fill_ = 0;
    uint64_t datagram_seq_ = 0;
    uint64_t since_announce_ = 0;
    uint64_t announces_ = 0;
    uint64_t send_errors_ = 0;
};
