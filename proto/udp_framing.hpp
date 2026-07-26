#pragma once
#include <cstddef>
#include <cstdint>
#include "messages.hpp"

// Datagram framing for the live feed. Shared by sender and receiver, so it
// lives in proto/ next to the message it wraps rather than in sim/.
//
// UDP is message-oriented and lossy: a receiver gets whole datagrams or
// nothing, never half of one, but it may miss some entirely and may see them
// out of order. The sequence number exists so loss is MEASURABLE rather than
// silent - without it, a dropped datagram is indistinguishable from a tick at
// which nothing was published.
struct UdpDatagramHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;   // records actually present, 1..kRecordsPerDatagram
    uint64_t datagram_seq;   // monotonic from the sender, gaps == loss
};
static_assert(sizeof(UdpDatagramHeader) == 16);

inline constexpr uint32_t kUdpMagic   = 0x50444F55U; // 'UODP' little-endian
inline constexpr uint16_t kUdpVersion = 1;

// 16 x 64B + 16B header = 1040 bytes, comfortably under the 1472-byte payload
// a 1500-byte Ethernet MTU leaves after IP and UDP headers. Staying under the
// MTU means the datagram is never IP-fragmented, so one lost frame costs one
// batch instead of silently destroying a larger reassembly.
inline constexpr size_t kRecordsPerDatagram = 16;
inline constexpr size_t kMaxDatagramBytes =
    sizeof(UdpDatagramHeader) + kRecordsPerDatagram * sizeof(OddsUpdate);
