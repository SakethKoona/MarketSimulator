// feed_dump: joins the multicast feed and prints decoded messages.
// Usage: feed_dump [group] [port] [iface]
#include "protocol/feed.hpp"
#include "protocol/moldudp64.hpp"
#include "udp_multicast.hpp"
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

template <typename T> static T load(const char *p) {
    T t;
    std::memcpy(&t, p, sizeof(T));
    return t;
}

static void print_msg(const char *p, std::uint16_t len) {
    using namespace feed;
    switch (static_cast<MsgType>(static_cast<std::uint8_t>(p[0]))) {
    case MsgType::SystemEvent: {
        auto m = load<SystemEvent>(p);
        std::printf("S  event=%c version=%u ts=%llu\n", m.event_code, m.version,
                    (unsigned long long)m.ts_ns);
        break;
    }
    case MsgType::StockDirectory: {
        auto m = load<StockDirectory>(p);
        std::printf("R  sym=%u ticker=%.8s scale=%u\n", m.symbol_id, m.ticker,
                    m.price_scale);
        break;
    }
    case MsgType::AddOrder: {
        auto m = load<AddOrder>(p);
        std::printf("A  seq=%llu sym=%u order=%llu %c %u@%llu\n",
                    (unsigned long long)m.book_seq, m.symbol_id,
                    (unsigned long long)m.order_id, m.side, m.qty,
                    (unsigned long long)m.price);
        break;
    }
    case MsgType::OrderExecuted: {
        auto m = load<OrderExecuted>(p);
        std::printf("E  seq=%llu sym=%u order=%llu %c exec=%u@%llu left=%u match=%llu\n",
                    (unsigned long long)m.book_seq, m.symbol_id,
                    (unsigned long long)m.order_id, m.side, m.exec_qty,
                    (unsigned long long)m.price, m.remaining_qty,
                    (unsigned long long)m.match_id);
        break;
    }
    case MsgType::OrderCancel: {
        auto m = load<OrderCancel>(p);
        std::printf("X  seq=%llu sym=%u order=%llu left=%u\n",
                    (unsigned long long)m.book_seq, m.symbol_id,
                    (unsigned long long)m.order_id, m.remaining_qty);
        break;
    }
    case MsgType::OrderDelete: {
        auto m = load<OrderDelete>(p);
        std::printf("D  seq=%llu sym=%u order=%llu\n", (unsigned long long)m.book_seq,
                    m.symbol_id, (unsigned long long)m.order_id);
        break;
    }
    case MsgType::OrderReplace: {
        auto m = load<OrderReplace>(p);
        std::printf("U  seq=%llu sym=%u order=%llu %c %u@%llu\n",
                    (unsigned long long)m.book_seq, m.symbol_id,
                    (unsigned long long)m.order_id, m.side, m.qty,
                    (unsigned long long)m.price);
        break;
    }
    default:
        std::printf("?  type=0x%02x len=%u\n", (unsigned)(unsigned char)p[0], len);
    }
}

int main(int argc, char **argv) {
    std::string group = argc > 1 ? argv[1] : "239.1.1.1";
    std::uint16_t port = argc > 2 ? std::stoi(argv[2]) : 30001;
    std::string iface = argc > 3 ? argv[3] : "";
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    net::UdpMulticastReceiver rx(group, port, iface);
    std::fprintf(stderr, "feed_dump: listening on %s:%u\n", group.c_str(), port);

    char buf[2048];
    std::uint64_t expect = 0, packets = 0, msgs = 0, gaps = 0, dups = 0;
    while (!g_stop) {
        ssize_t n = rx.recv(buf, sizeof(buf), 200);
        if (n <= 0)
            continue;
        mold::PacketReader pr(buf, static_cast<std::size_t>(n));
        if (!pr.valid())
            continue;
        ++packets;
        const auto &h = pr.header();
        if (h.message_count == mold::kEndOfSessionCount) {
            std::printf("-- end of session %.10s at seq %llu\n", h.session,
                        (unsigned long long)h.sequence_number);
            continue;
        }
        if (expect == 0)
            expect = h.sequence_number;
        if (h.message_count && h.sequence_number < expect) {
            ++dups;
            continue; // already seen (duplicate delivery)
        }
        if (h.sequence_number > expect) {
            ++gaps;
            std::printf("!! gap: expected %llu got %llu (%llu lost)\n",
                        (unsigned long long)expect, (unsigned long long)h.sequence_number,
                        (unsigned long long)(h.sequence_number - expect));
        }
        if (h.message_count == 0) {
            std::printf("hb  next=%llu\n", (unsigned long long)h.sequence_number);
        }
        std::uint64_t seq = h.sequence_number;
        pr.for_each([&](const char *p, std::uint16_t len) {
            std::printf("%8llu ", (unsigned long long)seq++);
            print_msg(p, len);
            ++msgs;
        });
        if (h.message_count)
            expect = h.sequence_number + h.message_count;
        std::fflush(stdout);
    }
    std::fprintf(stderr, "feed_dump: %llu packets, %llu messages, %llu gaps, %llu duplicate packets\n",
                 (unsigned long long)packets, (unsigned long long)msgs,
                 (unsigned long long)gaps, (unsigned long long)dups);
    return 0;
}
