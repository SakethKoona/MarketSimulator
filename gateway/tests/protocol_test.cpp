// Compile-time size checks live in the headers; this exercises framing.
#include "protocol/boe.hpp"
#include "protocol/feed.hpp"
#include "protocol/moldudp64.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    const char session[10] = {'2','0','2','6','1','0','0','8','A',' '};
    mold::PacketBuilder pb(session);

    // Fill a packet with Add messages until it refuses.
    pb.begin(100);
    feed::AddOrder add{};
    add.side = feed::kSideBuy; add.symbol_id = 0; add.order_id = 7;
    add.price = 105; add.qty = 5; add.book_seq = 2; add.ts_ns = 0;
    int n = 0;
    while (pb.append(add)) { ++n; add.order_id++; }
    assert(n == (1400 - 20) / (2 + 42));
    assert(pb.count() == n);
    assert(pb.size() <= mold::kMaxPacketSize);

    // Read it back.
    mold::PacketReader pr(pb.data(), pb.size());
    assert(pr.valid());
    assert(pr.header().sequence_number == 100);
    assert(std::memcmp(pr.header().session, session, 10) == 0);
    int seen = 0; std::uint64_t expect_id = 7;
    bool ok = pr.for_each([&](const char *p, std::uint16_t len) {
        assert(len == sizeof(feed::AddOrder));
        feed::AddOrder a; std::memcpy(&a, p, len);
        assert(a.type == 'A' && a.order_id == expect_id && a.price == 105);
        ++seen; ++expect_id;
    });
    assert(ok && seen == n);

    // Truncated packet is detected.
    mold::PacketReader bad(pb.data(), pb.size() - 1);
    assert(!bad.for_each([](const char *, std::uint16_t) {}));

    // Reference encoding from the spec, section 6.
    feed::AddOrder ref{}; ref.side='B'; ref.symbol_id=0; ref.order_id=7;
    ref.price=105; ref.qty=5; ref.book_seq=2; ref.ts_ns=0;
    const unsigned char want[42] = {
        0x41,0x42, 0,0,0,0, 7,0,0,0,0,0,0,0, 0x69,0,0,0,0,0,0,0,
        5,0,0,0, 2,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
    assert(std::memcmp(&ref, want, 42) == 0);

    // Heartbeat / end of session.
    pb.control(205, 0); assert(pb.size() == 20 && pb.count() == 0);
    pb.control(205, mold::kEndOfSessionCount);
    mold::PacketReader eos(pb.data(), pb.size());
    assert(eos.for_each([](const char *, std::uint16_t) { assert(false); }));

    // BOE header length arithmetic.
    boe::Header h{}; h.message_type = (std::uint8_t)boe::MsgType::NewOrder;
    h.message_length = boe::kHeaderTail + sizeof(boe::NewOrder);
    assert(h.message_length == 49);
    assert(sizeof(boe::Header) + sizeof(boe::NewOrder) ==
           2 + 2 + h.message_length);

    std::puts("protocol_test: OK");
    return 0;
}
