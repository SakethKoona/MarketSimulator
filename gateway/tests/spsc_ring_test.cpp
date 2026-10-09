#include "spsc_ring.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <variant>

struct Tracked {
    static int live;
    int v;
    explicit Tracked(int x) : v(x) { ++live; }
    Tracked(const Tracked &o) : v(o.v) { ++live; }
    Tracked(Tracked &&o) noexcept : v(o.v) { ++live; }
    Tracked &operator=(const Tracked &) = default;
    Tracked &operator=(Tracked &&) noexcept = default;
    ~Tracked() { --live; }
};
int Tracked::live = 0;

int main() {
    // Capacity, drop policy, destructor accounting.
    {
        SpscRing<Tracked> r(4);
        for (int i = 0; i < 4; ++i) assert(r.try_push(Tracked(i)));
        assert(!r.try_push(Tracked(99)));
        assert(r.dropped() == 1);
        assert(r.size_approx() == 4);
        Tracked out(-1);
        for (int i = 0; i < 4; ++i) { assert(r.try_pop(out)); assert(out.v == i); }
        assert(!r.try_pop(out));
        assert(r.try_push(Tracked(5)));
        assert(r.peek() && r.peek()->v == 5);
        r.pop();
        assert(r.peek() == nullptr);
    }
    assert(Tracked::live == 0);

    // Leftover elements are destroyed by the ring destructor.
    {
        SpscRing<Tracked> r(8);
        for (int i = 0; i < 5; ++i) r.try_push(Tracked(i));
        assert(Tracked::live == 5);
    }
    assert(Tracked::live == 0);

    // Non-trivial payloads (what EventSink will store).
    {
        using V = std::variant<int, std::string>;
        SpscRing<V> r(2);
        assert(r.try_push(V(std::string(100, 'x'))));
        assert(r.try_push(V(7)));
        V out;
        assert(r.try_pop(out) && std::get<std::string>(out).size() == 100);
        assert(r.try_pop(out) && std::get<int>(out) == 7);
    }

    // Two threads, wraparound many times, strict FIFO, no loss when the
    // consumer keeps up (producer spins on full instead of dropping).
    {
        constexpr std::uint64_t N = 2'000'000;
        SpscRing<std::uint64_t> r(1024);
        std::thread prod([&] {
            for (std::uint64_t i = 0; i < N; ++i)
                while (!r.try_push(i)) {}
        });
        std::uint64_t expect = 0, x;
        while (expect < N)
            if (r.try_pop(x)) { assert(x == expect); ++expect; }
        prod.join();
        assert(r.size_approx() == 0);
        std::printf("spsc_ring_test: %llu items FIFO across threads, drops while spinning=%llu\n",
                    (unsigned long long)N, (unsigned long long)r.dropped());
    }
    std::puts("spsc_ring_test: OK");
    return 0;
}
