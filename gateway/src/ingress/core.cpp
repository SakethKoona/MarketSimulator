#include "ingress/core.hpp"
#include "common.hpp"
#include <cstdio>
#include <cstring>
#include <optional>

namespace {
OrderEntryCore *core_of(mktsim_exchange *ex) { return reinterpret_cast<OrderEntryCore *>(ex); }

mktsim_session *c_open_session(mktsim_exchange *ex, const char *name) { return core_of(ex)->open_session(name); }
int c_resolve_symbol(mktsim_exchange *ex, const char *t, size_t len, uint32_t *out) {
    return core_of(ex)->resolve_symbol(t, len, out);
}
size_t c_symbols(mktsim_exchange *ex, mktsim_symbol_info *out, size_t max) { return core_of(ex)->symbols(out, max); }
uint64_t c_now_ns(void) { return get_current_timestamp(); }
void c_log(mktsim_exchange *ex, const char *adapter, const char *msg) { core_of(ex)->log(adapter, msg); }

// Sessions carry a back-pointer so the session-only entry points can find
// their core without a global. The opaque mktsim_session is a SessionHandle*.
struct SessionHandle {
    OrderEntryCore *core;
    void *session;
};
void c_close_session(mktsim_session *s) {
    auto *h = reinterpret_cast<SessionHandle *>(s);
    h->core->close_session(s);
}
int c_submit(mktsim_session *s, const mktsim_order_req *req) {
    auto *h = reinterpret_cast<SessionHandle *>(s);
    return h->core->submit(s, *req);
}
size_t c_poll(mktsim_session *s, mktsim_report_fn cb, void *user, size_t max) {
    auto *h = reinterpret_cast<SessionHandle *>(s);
    return h->core->poll(s, cb, user, max);
}
} // namespace

OrderEntryCore::OrderEntryCore(Exchange &ex, Config cfg)
    : ex_(ex), cfg_(cfg), free_slots_(1) {
    const std::size_t shards = ex_.NumShards();
    for (std::size_t i = 0; i < shards; ++i) {
        cmds_.push_back(std::make_unique<MpmcRing<Command>>(cfg_.cmd_ring));
        owners_.emplace_back();
    }
    // Symbol table.
    std::uint32_t n = 0;
    for (const auto &[name, id] : ex_.Symbols())
        n = std::max<std::uint32_t>(n, static_cast<std::uint32_t>(id) + 1);
    symbol_shard_.assign(n, 0);
    tickers_.assign(n, "");
    for (const auto &[name, id] : ex_.Symbols()) {
        symbol_shard_[id] = static_cast<std::uint8_t>(ex_.ShardOf(id));
        tickers_[id] = name;
        by_ticker_[name] = static_cast<std::uint32_t>(id);
    }
    // Session slots: a power-of-two ring of free slot numbers.
    std::size_t pow2 = 1;
    while (pow2 < cfg_.max_sessions)
        pow2 <<= 1;
    // Rebuild the free-slot ring at the right size (constructed with 1 above).
    const_cast<MpmcRing<std::uint32_t> &>(free_slots_).~MpmcRing();
    new (&free_slots_) MpmcRing<std::uint32_t>(pow2);
    for (std::uint32_t i = 0; i < cfg_.max_sessions; ++i) {
        auto s = std::make_unique<Session>();
        s->slot = i;
        s->reports = std::make_unique<MpmcRing<mktsim_report>>(cfg_.report_ring);
        sessions_.push_back(std::move(s));
        free_slots_.try_push(i);
    }
    api_.version = MKTSIM_INGRESS_API_VERSION;
    api_.open_session = &c_open_session;
    api_.close_session = &c_close_session;
    api_.submit = &c_submit;
    api_.poll = &c_poll;
    api_.resolve_symbol = &c_resolve_symbol;
    api_.symbols = &c_symbols;
    api_.now_ns = &c_now_ns;
    api_.log = &c_log;
}

OrderEntryCore::~OrderEntryCore() = default;

// ---------------------------------------------------------------- sessions

mktsim_session *OrderEntryCore::open_session(const char *name) {
    std::uint32_t slot;
    if (!free_slots_.try_pop(slot))
        return nullptr;
    Session &s = *sessions_[slot];
    // Drain anything a previous occupant left behind, then bump the
    // generation so stale reports addressed to it are dropped.
    mktsim_report junk;
    while (s.reports->try_pop(junk)) {
    }
    s.gen.fetch_add(1, std::memory_order_acq_rel);
    s.name = name ? name : "";
    s.open.store(true, std::memory_order_release);
    stats_.sessions_opened.fetch_add(1, std::memory_order_relaxed);
    stats_.sessions_open.fetch_add(1, std::memory_order_relaxed);
    auto *h = new SessionHandle{this, &s};
    return reinterpret_cast<mktsim_session *>(h);
}

void OrderEntryCore::close_session(mktsim_session *sh) {
    auto *h = reinterpret_cast<SessionHandle *>(sh);
    Session &s = *static_cast<Session *>(h->session);
    if (s.open.exchange(false, std::memory_order_acq_rel)) {
        stats_.sessions_open.fetch_sub(1, std::memory_order_relaxed);
        free_slots_.try_push(s.slot);
    }
    delete h;
}

bool OrderEntryCore::alive(Owner o) const {
    const Session &s = *sessions_[o.slot];
    return s.open.load(std::memory_order_acquire) && s.gen.load(std::memory_order_acquire) == o.gen;
}

// ---------------------------------------------------------------- submit / poll

int OrderEntryCore::submit(mktsim_session *sh, const mktsim_order_req &req) {
    auto *h = reinterpret_cast<SessionHandle *>(sh);
    Session &s = *static_cast<Session *>(h->session);
    if (!s.open.load(std::memory_order_acquire))
        return MKTSIM_ECLOSED;
    std::size_t shard;
    switch (req.kind) {
    case MKTSIM_REQ_NEW:
        if (req.symbol_id >= symbol_shard_.size()) {
            stats_.bad_request.fetch_add(1, std::memory_order_relaxed);
            return MKTSIM_EBADSYMBOL;
        }
        if (req.qty == 0 || (req.side != MKTSIM_BUY && req.side != MKTSIM_SELL) ||
            (req.ord_type != MKTSIM_MARKET && req.ord_type != MKTSIM_LIMIT) ||
            (req.tif != MKTSIM_GTC && req.tif != MKTSIM_IOC && req.tif != MKTSIM_FOK) ||
            (req.ord_type == MKTSIM_LIMIT && req.price == 0) ||
            (req.ord_type == MKTSIM_MARKET && req.tif == MKTSIM_GTC)) {
            stats_.bad_request.fetch_add(1, std::memory_order_relaxed);
            return MKTSIM_EBADREQ;
        }
        shard = symbol_shard_[req.symbol_id];
        break;
    case MKTSIM_REQ_CANCEL:
    case MKTSIM_REQ_MODIFY:
        if (req.order_id == 0 || (req.kind == MKTSIM_REQ_MODIFY && req.qty == 0)) {
            stats_.bad_request.fetch_add(1, std::memory_order_relaxed);
            return MKTSIM_EBADREQ;
        }
        shard = shard_of(req.order_id);
        if (shard >= cmds_.size()) {
            stats_.bad_request.fetch_add(1, std::memory_order_relaxed);
            return MKTSIM_EBADREQ;
        }
        break;
    default:
        stats_.bad_request.fetch_add(1, std::memory_order_relaxed);
        return MKTSIM_EBADREQ;
    }
    Command c{req, Owner{s.slot, s.gen.load(std::memory_order_acquire)}};
    if (!cmds_[shard]->try_push(c)) {
        stats_.busy.fetch_add(1, std::memory_order_relaxed);
        return MKTSIM_EBUSY;
    }
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);
    return MKTSIM_OK;
}

std::size_t OrderEntryCore::poll(mktsim_session *sh, mktsim_report_fn cb, void *user, std::size_t max) {
    auto *h = reinterpret_cast<SessionHandle *>(sh);
    Session &s = *static_cast<Session *>(h->session);
    std::size_t n = 0;
    mktsim_report r;
    while (n < max && s.reports->try_pop(r)) {
        cb(user, &r);
        ++n;
    }
    return n;
}

int OrderEntryCore::resolve_symbol(const char *ticker, std::size_t len, std::uint32_t *out) const {
    while (len && (ticker[len - 1] == ' ' || ticker[len - 1] == '\0'))
        --len;
    auto it = by_ticker_.find(std::string(ticker, len));
    if (it == by_ticker_.end())
        return MKTSIM_EBADSYMBOL;
    *out = it->second;
    return MKTSIM_OK;
}

std::size_t OrderEntryCore::symbols(mktsim_symbol_info *out, std::size_t max) const {
    std::size_t n = 0;
    for (std::uint32_t id = 0; id < tickers_.size() && n < max; ++id) {
        if (tickers_[id].empty())
            continue;
        out[n].symbol_id = id;
        std::snprintf(out[n].ticker, sizeof(out[n].ticker), "%s", tickers_[id].c_str());
        ++n;
    }
    return n;
}

void OrderEntryCore::log(const char *adapter, const char *msg) const {
    std::fprintf(stderr, "ingress[%s]: %s\n", adapter ? adapter : "?", msg ? msg : "");
}

// ---------------------------------------------------------------- engine side

void OrderEntryCore::push_report(Owner o, mktsim_report r) {
    if (!alive(o))
        return;
    r.ts_ns = get_current_timestamp();
    if (!sessions_[o.slot]->reports->try_push(r))
        stats_.report_drops.fetch_add(1, std::memory_order_relaxed);
    else
        stats_.reports.fetch_add(1, std::memory_order_relaxed);
}

bool OrderEntryCore::on_book(std::uint32_t symbol_id, std::uint64_t order_id) const {
    if (symbol_id >= symbol_shard_.size())
        return false;
    return ex_.GetBook(symbol_id).FindOrder(order_id) != nullptr;
}

void OrderEntryCore::drain_fills(std::size_t shard) {
    TradeFillEvent f;
    auto &owners = owners_[shard];
    while (ex_.Sink(shard).fills().try_pop(f)) {
        mktsim_report r{};
        r.kind = MKTSIM_RPT_EXECUTION;
        r.status = MKTSIM_ST_OK;
        r.symbol_id = static_cast<std::uint32_t>(f.symbol_id);
        r.price = f.price;
        r.last_qty = static_cast<std::uint32_t>(f.qty);
        r.match_id = f.trade_id;
        // Resting side.
        if (auto it = owners.find(f.resting_id); it != owners.end()) {
            r.order_id = f.resting_id;
            r.side = f.aggressor_side == Side::Buy ? MKTSIM_SELL : MKTSIM_BUY;
            push_report(it->second, r);
            if (!on_book(r.symbol_id, f.resting_id))
                owners.erase(it);
        }
        // Aggressor side; its owner entry is retired by the NEW/MODIFY path.
        if (auto it = owners.find(f.aggressor_id); it != owners.end()) {
            r.order_id = f.aggressor_id;
            r.side = f.aggressor_side == Side::Buy ? MKTSIM_BUY : MKTSIM_SELL;
            push_report(it->second, r);
        }
    }
}

std::size_t OrderEntryCore::pump(std::size_t shard, std::size_t max_commands) {
    auto &ring = *cmds_[shard];
    auto &owners = owners_[shard];
    std::size_t n = 0;
    Command c;
    while (n < max_commands && ring.try_pop(c)) {
        ++n;
        stats_.applied.fetch_add(1, std::memory_order_relaxed);
        const mktsim_order_req &q = c.req;
        if (!alive(c.owner))
            continue; // session went away; nothing to report to
        switch (q.kind) {
        case MKTSIM_REQ_NEW: {
            FillResult res = ex_.SubmitOrder(
                q.symbol_id, q.price, q.qty, q.side == MKTSIM_BUY ? Side::Buy : Side::Sell,
                q.ord_type == MKTSIM_MARKET ? OrderType::MARKET : OrderType::LIMIT,
                q.tif == MKTSIM_IOC ? TypeInForce::IOC : q.tif == MKTSIM_FOK ? TypeInForce::FOK : TypeInForce::GTC);
            mktsim_report r{};
            r.request_id = q.request_id;
            r.order_id = res.order_id;
            r.side = q.side;
            r.symbol_id = q.symbol_id;
            r.qty = q.qty;
            r.price = q.price;
            r.status = static_cast<std::uint8_t>(res.status_code);
            if (res.fill_status == FillStatus::Rejected) {
                r.kind = MKTSIM_RPT_REJECTED;
                push_report(c.owner, r);
                break;
            }
            owners[res.order_id] = c.owner;
            r.kind = MKTSIM_RPT_ACCEPTED;
            r.leaves_qty = res.resting ? static_cast<std::uint32_t>(res.qty_remaining) : 0;
            push_report(c.owner, r);
            drain_fills(shard); // executions follow the acknowledgment
            if (!res.resting) {
                if (res.qty_remaining > 0) {
                    mktsim_report k{};
                    k.order_id = res.order_id;
                    k.kind = MKTSIM_RPT_CANCELLED;
                    k.status = q.tif == MKTSIM_FOK ? MKTSIM_ST_FOK_FAILED : MKTSIM_ST_OK;
                    k.symbol_id = q.symbol_id;
                    push_report(c.owner, k);
                }
                owners.erase(res.order_id);
            }
            break;
        }
        case MKTSIM_REQ_CANCEL: {
            StatusCode sc = ex_.CancelOrder(q.order_id);
            mktsim_report r{};
            r.request_id = q.request_id;
            r.order_id = q.order_id;
            r.status = static_cast<std::uint8_t>(sc);
            r.kind = sc == StatusCode::Success ? MKTSIM_RPT_CANCELLED : MKTSIM_RPT_REJECTED;
            push_report(c.owner, r);
            if (sc == StatusCode::Success)
                owners.erase(q.order_id);
            break;
        }
        case MKTSIM_REQ_MODIFY: {
            std::optional<Price> px;
            if (q.price)
                px = q.price;
            StatusCode sc = ex_.ModifyOrder(q.order_id, q.qty, px);
            mktsim_report r{};
            r.request_id = q.request_id;
            r.order_id = q.order_id;
            r.status = static_cast<std::uint8_t>(sc);
            r.qty = q.qty;
            r.price = q.price;
            r.leaves_qty = q.qty;
            r.kind = sc == StatusCode::Success ? MKTSIM_RPT_MODIFIED : MKTSIM_RPT_REJECTED;
            push_report(c.owner, r);
            if (sc == StatusCode::Success) {
                drain_fills(shard); // a crossing replace executes right away
                auto it = owners.find(q.order_id);
                if (it != owners.end()) {
                    std::uint32_t sym = 0;
                    // Order ids don't carry the symbol; find it via the shard's books.
                    bool found = false;
                    for (std::uint32_t s = 0; s < symbol_shard_.size() && !found; ++s)
                        if (symbol_shard_[s] == shard && on_book(s, q.order_id)) {
                            sym = s;
                            found = true;
                        }
                    (void)sym;
                    if (!found)
                        owners.erase(it);
                }
            }
            break;
        }
        default:
            break;
        }
    }
    drain_fills(shard);
    return n;
}
