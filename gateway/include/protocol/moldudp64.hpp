#pragma once
// MoldUDP64-style framing, little-endian variant. Contract: docs/protocol/feed-v1.md §2
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mold {

constexpr std::size_t kSessionLen = 10;
constexpr std::size_t kMaxPacketSize = 1400;
constexpr std::uint16_t kEndOfSessionCount = 0xFFFF;

#pragma pack(push, 1)
struct PacketHeader {
    char session[kSessionLen];
    std::uint64_t sequence_number;
    std::uint16_t message_count;
};
static_assert(sizeof(PacketHeader) == 20);

struct RetransmitRequest {
    char session[kSessionLen];
    std::uint64_t sequence_number;
    std::uint16_t message_count;
};
static_assert(sizeof(RetransmitRequest) == 20);
#pragma pack(pop)

constexpr std::size_t kMaxPayloadPerPacket = kMaxPacketSize - sizeof(PacketHeader);

// Builds one outbound packet in a caller-owned buffer. Not thread-safe; one
// per publisher thread. Usage: begin(seq) → append(...)* → finish() → send.
class PacketBuilder {
  public:
    explicit PacketBuilder(const char session[kSessionLen]) {
        std::memcpy(header().session, session, kSessionLen);
        header().sequence_number = 0;
        header().message_count = 0;
        size_ = sizeof(PacketHeader);
    }

    void begin(std::uint64_t first_seq) {
        header().sequence_number = first_seq;
        header().message_count = 0;
        size_ = sizeof(PacketHeader);
    }

    // Returns false (and appends nothing) when the message would not fit.
    bool append(const void *msg, std::uint16_t len) {
        if (size_ + sizeof(std::uint16_t) + len > kMaxPacketSize)
            return false;
        std::memcpy(buf_ + size_, &len, sizeof(len));
        size_ += sizeof(len);
        std::memcpy(buf_ + size_, msg, len);
        size_ += len;
        header().message_count++;
        return true;
    }

    template <typename Msg> bool append(const Msg &m) {
        return append(&m, static_cast<std::uint16_t>(sizeof(Msg)));
    }

    // A heartbeat (count 0) or end-of-session (count 0xFFFF) packet.
    void control(std::uint64_t next_seq, std::uint16_t count) {
        header().sequence_number = next_seq;
        header().message_count = count;
        size_ = sizeof(PacketHeader);
    }

    bool empty() const { return header().message_count == 0; }
    std::uint16_t count() const { return header().message_count; }
    std::uint64_t first_seq() const { return header().sequence_number; }
    const char *data() const { return buf_; }
    std::size_t size() const { return size_; }

  private:
    PacketHeader &header() { return *reinterpret_cast<PacketHeader *>(buf_); }
    const PacketHeader &header() const {
        return *reinterpret_cast<const PacketHeader *>(buf_);
    }
    alignas(8) char buf_[kMaxPacketSize];
    std::size_t size_ = 0;
};

// Walks the message blocks of a received packet.
class PacketReader {
  public:
    PacketReader(const char *data, std::size_t len) : data_(data), len_(len) {}

    bool valid() const { return len_ >= sizeof(PacketHeader); }
    const PacketHeader &header() const {
        return *reinterpret_cast<const PacketHeader *>(data_);
    }

    // Calls fn(const char* payload, uint16_t len) for each block. Returns
    // false if the packet was truncated.
    template <typename Fn> bool for_each(Fn &&fn) const {
        std::size_t off = sizeof(PacketHeader);
        std::uint16_t n = header().message_count;
        if (n == kEndOfSessionCount)
            return true;
        for (std::uint16_t i = 0; i < n; ++i) {
            if (off + sizeof(std::uint16_t) > len_)
                return false;
            std::uint16_t l;
            std::memcpy(&l, data_ + off, sizeof(l));
            off += sizeof(l);
            if (off + l > len_)
                return false;
            fn(data_ + off, l);
            off += l;
        }
        return true;
    }

  private:
    const char *data_;
    std::size_t len_;
};

} // namespace mold
