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
enum class UdpDatagramType : uint8_t {
    Data     = 0,   // payload is record_count OddsUpdate records
    Announce = 1,   // payload is exactly one AnnounceRecord
};

// Still 16 bytes after gaining a type: record_count is capped at
// kRecordsPerDatagram (16) and never needed 16 bits.
struct UdpDatagramHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t  type;           // UdpDatagramType
    uint8_t  record_count;   // Data: 1..kRecordsPerDatagram. Announce: always 1.
    uint64_t datagram_seq;   // monotonic across BOTH types; gaps == loss
};
static_assert(sizeof(UdpDatagramHeader) == 16);

inline constexpr uint32_t kUdpMagic   = 0x50444F55U; // 'UODP' little-endian
inline constexpr uint16_t kUdpVersion = 2;           // v1 had no type field

// What a session file carries in its header, minus what a stream cannot know.
//
// A live consumer joins mid-stream and has no beginning to read, so the
// stream has to keep telling it what it is. This is what real market-data
// feeds do (an instrument directory repeated on a cycle) and the reason to
// prefer it over discovering markets/books lazily from arriving records:
// lazy discovery makes the engine's table sizing depend on which records it
// happened to see first, which is unreproducible and makes bugs unrepeatable.
//
// tick_interval_ns is on the wire rather than assumed. Four files used to
// hold private copies of that constant and it caused a real build failure;
// a receiver on another host would be a fifth copy with no compiler to catch
// the drift.
struct AnnounceRecord {
    uint64_t seed;
    uint64_t tick_interval_ns;
    uint32_t markets;
    uint32_t books;
    uint32_t record_size;    // sizeof(OddsUpdate), same check SessionHeader makes
    uint32_t _pad;
};
static_assert(sizeof(AnnounceRecord) == 32);

// 16 x 64B + 16B header = 1040 bytes, comfortably under the 1472-byte payload
// a 1500-byte Ethernet MTU leaves after IP and UDP headers. Staying under the
// MTU means the datagram is never IP-fragmented, so one lost frame costs one
// batch instead of silently destroying a larger reassembly.
inline constexpr size_t kRecordsPerDatagram = 16;
inline constexpr size_t kMaxDatagramBytes =
    sizeof(UdpDatagramHeader) + kRecordsPerDatagram * sizeof(OddsUpdate);

// One announce per this many data datagrams, plus one before any data. Sets
// the worst-case wait for a consumer joining mid-stream.
inline constexpr uint64_t kAnnounceInterval = 64;
