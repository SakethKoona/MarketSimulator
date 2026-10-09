#include "shell.hpp"
#include "orderbook.hpp"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

std::string lower(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool parse_u64(const std::string &s, uint64_t &v) {
    if (s.empty())
        return false;
    uint64_t acc = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        uint64_t next = acc * 10 + static_cast<uint64_t>(c - '0');
        if (next < acc)
            return false;
        acc = next;
    }
    v = acc;
    return true;
}

const char *status_name(StatusCode s) {
    switch (s) {
    case StatusCode::Success: return "ok";
    case StatusCode::SymbolNotFound: return "symbol not found";
    case StatusCode::OrderNotFound: return "order not found";
    case StatusCode::Failed: return "failed";
    case StatusCode::NotEnoughLiquidity: return "not enough liquidity";
    case StatusCode::FOKFailed: return "fok: cannot fill all";
    case StatusCode::InvalidPrice: return "invalid price";
    case StatusCode::InvalidQuantity: return "invalid quantity";
    case StatusCode::DuplicateOrder: return "duplicate order";
    }
    return "unknown";
}

const char *side_name(Side s) { return s == Side::Buy ? "buy" : "sell"; }

} // namespace

Shell::Shell(Session &session, std::ostream &out, std::ostream &err)
    : s_(session), out_(out), err_(err) {}

bool Shell::fail(const std::string &msg) {
    err_ << "error: " << msg << "\n";
    failures++;
    return true;
}

bool Shell::need_inspect() {
    if (s_.CanInspect())
        return true;
    fail("book inspection is only available in-process (shell/run), not over "
         "a connection");
    return false;
}

int Shell::Repl(std::istream &in, bool interactive) {
    std::string line;
    while (true) {
        if (interactive) {
            out_ << COLORS::bold << "mktsim> " << COLORS::reset << std::flush;
        }
        if (!std::getline(in, line))
            break;
        if (!Execute(line))
            break;
    }
    if (interactive)
        out_ << "\n";
    return failures;
}

bool Shell::Execute(const std::string &raw) {
    std::string line = raw.substr(0, raw.find('#'));
    std::istringstream ss(line);
    Args a;
    std::string tok;
    while (ss >> tok)
        a.push_back(tok);
    if (a.empty())
        return true;

    std::string cmd = lower(a[0]);
    if (cmd == "quit" || cmd == "exit" || cmd == "q")
        return false;

    // expect ok|reject CMD ... : assert the outcome of an order command
    if (cmd == "expect") {
        if (a.size() < 3 || (lower(a[1]) != "ok" && lower(a[1]) != "reject"))
            return fail("usage: expect ok|reject COMMAND ...");
        bool want_ok = lower(a[1]) == "ok";
        std::string rest;
        for (std::size_t i = 2; i < a.size(); i++)
            rest += (i > 2 ? " " : "") + a[i];
        have_result_ = false;
        int before = failures;
        bool cont = Execute(rest);
        if (!have_result_) {
            if (failures == before)
                fail("expect: '" + rest + "' is not an order command");
            return cont;
        }
        if (last_ok_ != want_ok)
            fail(std::string("expected ") + (want_ok ? "ok" : "reject") +
                 " but got " + (last_ok_ ? "ok" : "reject") + ": " + rest);
        return cont;
    }
    if (cmd == "help" || cmd == "?") { help(); return true; }
    if (cmd == "buy") return cmd_order(a, Side::Buy);
    if (cmd == "sell") return cmd_order(a, Side::Sell);
    if (cmd == "cancel") return cmd_cancel(a);
    if (cmd == "modify") return cmd_modify(a);
    if (cmd == "book") return cmd_book(a, false);
    if (cmd == "l2") return cmd_book(a, true);
    if (cmd == "top") return cmd_top();
    if (cmd == "order") return cmd_order_info(a);
    if (cmd == "events") return cmd_events(a);
    if (cmd == "flow") return cmd_flow(a);
    if (cmd == "symbols") return cmd_symbols();
    if (cmd == "echo") {
        if (a.size() != 2 || (lower(a[1]) != "on" && lower(a[1]) != "off"))
            return fail("usage: echo on|off");
        echo_events = lower(a[1]) == "on";
        return true;
    }
    return fail("unknown command '" + a[0] + "' (try help)");
}

void Shell::help() {
    out_ << "orders\n"
            "  buy  SYM QTY @ PRICE [ioc|fok]   limit (GTC by default)\n"
            "  sell SYM QTY @ PRICE [ioc|fok]\n"
            "  buy  SYM QTY market              market order\n"
            "  cancel ID\n"
            "  modify ID QTY                    reduce qty, keeps priority\n"
            "  modify ID QTY @ PRICE            cancel/replace (new priority)\n"
            "inspect (in-process only)\n"
            "  book SYM            L3: every order per level\n"
            "  l2 SYM [DEPTH]      aggregated levels\n"
            "  top                 best bid/ask per symbol\n"
            "  order ID            one order's state\n"
            "  events [N]          last N events (default: all pending)\n"
            "  flow SYM N [SEED]   run N synthetic ops on SYM\n"
            "  symbols\n"
            "session\n"
            "  echo on|off         print events after every command\n"
            "  expect ok|reject CMD   assert an order command's outcome\n"
            "  help, quit\n"
            "In run mode, parse errors and failed expectations make the exit\n"
            "status non-zero; plain rejections do not.\n";
}

bool Shell::cmd_order(const Args &a, Side side) {
    // buy SYM QTY @ PRICE [ioc|fok]   |   buy SYM QTY market
    if (a.size() < 4)
        return fail("usage: " + lower(a[0]) + " SYM QTY @ PRICE [ioc|fok] | " +
                    lower(a[0]) + " SYM QTY market");
    std::string sym = upper(a[1]);
    uint64_t qty;
    if (!parse_u64(a[2], qty))
        return fail("bad quantity '" + a[2] + "'");

    OrderType type = OrderType::LIMIT;
    TypeInForce tif = TypeInForce::GTC;
    uint64_t price = 0;

    if (lower(a[3]) == "market") {
        type = OrderType::MARKET;
        tif = TypeInForce::IOC;
        if (a.size() > 4)
            return fail("market orders take no price or tif");
    } else {
        if (a[3] != "@" || a.size() < 5)
            return fail("limit orders need '@ PRICE'");
        if (!parse_u64(a[4], price))
            return fail("bad price '" + a[4] + "'");
        if (a.size() > 5) {
            std::string t = lower(a[5]);
            if (t == "ioc") tif = TypeInForce::IOC;
            else if (t == "fok") tif = TypeInForce::FOK;
            else if (t == "gtc") tif = TypeInForce::GTC;
            else return fail("bad time in force '" + a[5] + "' (ioc|fok|gtc)");
        }
        if (a.size() > 6)
            return fail("too many arguments");
    }

    OrderOutcome o = s_.Submit(sym, side, qty, price, type, tif);
    report(o);
    return true;
}

void Shell::report(const OrderOutcome &o) {
    note_result(o.status == StatusCode::Success);
    if (o.status != StatusCode::Success) {
        out_ << COLORS::red << "rejected" << COLORS::reset << ": "
             << status_name(o.status);
        if (!o.detail.empty())
            out_ << " (" << o.detail << ")";
        out_ << "\n";
        return;
    }
    out_ << COLORS::green << "order " << o.order_id << COLORS::reset;
    switch (o.fill) {
    case FillStatus::Accepted:
        out_ << " resting " << o.remaining;
        break;
    case FillStatus::FullyFilled:
        out_ << " filled " << o.executed;
        break;
    case FillStatus::PartiallyFilled:
        out_ << " filled " << o.executed << ", "
             << (o.resting ? "resting " : "cancelled ") << o.remaining;
        break;
    case FillStatus::Rejected:
        out_ << " nothing done";
        break;
    }
    out_ << "\n";
    print_fills();
    if (echo_events)
        print_events(0);
}

void Shell::print_fills() {
    for (const Fill &f : s_.TakeFills()) {
        out_ << "  " << COLORS::dim << "fill" << COLORS::reset << " order "
             << f.order_id << ": " << f.qty << " @ " << f.price << " (match "
             << f.match_id << ")\n";
    }
}

bool Shell::cmd_cancel(const Args &a) {
    uint64_t id;
    if (a.size() != 2 || !parse_u64(a[1], id))
        return fail("usage: cancel ID");
    StatusCode sc = s_.Cancel(id);
    note_result(sc == StatusCode::Success);
    if (sc != StatusCode::Success) {
        out_ << COLORS::red << "rejected" << COLORS::reset << ": cancel "
             << a[1] << ": " << status_name(sc) << "\n";
        return true;
    }
    out_ << "order " << id << " cancelled\n";
    if (echo_events)
        print_events(0);
    return true;
}

bool Shell::cmd_modify(const Args &a) {
    // modify ID QTY [@ PRICE]
    uint64_t id, qty, price;
    std::optional<Price> px;
    if (a.size() < 3 || !parse_u64(a[1], id) || !parse_u64(a[2], qty))
        return fail("usage: modify ID QTY [@ PRICE]");
    if (a.size() == 5 && a[3] == "@" && parse_u64(a[4], price))
        px = price;
    else if (a.size() != 3)
        return fail("usage: modify ID QTY [@ PRICE]");
    StatusCode sc = s_.Modify(id, qty, px);
    note_result(sc == StatusCode::Success);
    if (sc != StatusCode::Success) {
        out_ << COLORS::red << "rejected" << COLORS::reset << ": modify "
             << a[1] << ": " << status_name(sc) << "\n";
        return true;
    }
    out_ << "order " << id << " modified\n";
    print_fills();
    if (echo_events)
        print_events(0);
    return true;
}

bool Shell::cmd_book(const Args &a, bool l2) {
    if (!need_inspect())
        return true;
    if (a.size() < 2)
        return fail(l2 ? "usage: l2 SYM [DEPTH]" : "usage: book SYM");
    Exchange &ex = *s_.exchange();
    auto sym = ex.ResolveSymbol(upper(a[1]));
    if (!sym)
        return fail("unknown symbol '" + a[1] + "'");

    if (!l2) {
        out_ << COLORS::bold << upper(a[1]) << " (symbol " << *sym << ")"
             << COLORS::reset;
        ex.DisplayBook(*sym);
        return true;
    }

    uint64_t depth = 10;
    if (a.size() > 2 && !parse_u64(a[2], depth))
        return fail("bad depth");

    const OrderBook &book = ex.GetBook(*sym);
    std::vector<const PriceLevel *> asks, bids;
    for (auto *n = book.asks().GetHead(); n && asks.size() < depth; n = n->forward[0])
        asks.push_back(&n->value);
    for (auto *n = book.bids().GetHead(); n && bids.size() < depth; n = n->forward[0])
        bids.push_back(&n->value);

    out_ << COLORS::bold << upper(a[1]) << COLORS::reset << "\n";
    out_ << std::setw(10) << "price" << std::setw(10) << "qty" << std::setw(8)
         << "orders" << "\n";
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        out_ << COLORS::red << std::setw(10) << (*it)->price << COLORS::reset
             << std::setw(10) << (*it)->TotalQuantity() << std::setw(8)
             << (*it)->GetSize() << "\n";
    if (!asks.empty() && !bids.empty())
        out_ << COLORS::dim << std::setw(10) << "---" << " spread "
             << (asks.front()->price - bids.front()->price) << COLORS::reset
             << "\n";
    else if (asks.empty() || bids.empty())
        out_ << COLORS::dim << std::setw(10) << "---" << COLORS::reset << "\n";
    for (auto *lv : bids)
        out_ << COLORS::green << std::setw(10) << lv->price << COLORS::reset
             << std::setw(10) << lv->TotalQuantity() << std::setw(8)
             << lv->GetSize() << "\n";
    return true;
}

bool Shell::cmd_top() {
    if (!need_inspect())
        return true;
    Exchange &ex = *s_.exchange();
    out_ << std::left << std::setw(8) << "symbol" << std::right
         << std::setw(10) << "bid qty" << std::setw(10) << "bid"
         << std::setw(10) << "ask" << std::setw(10) << "ask qty"
         << std::setw(8) << "spread" << "\n";
    for (const std::string &t : s_.Tickers()) {
        SymbolId sym = *ex.ResolveSymbol(t);
        const OrderBook &book = ex.GetBook(sym);
        const PriceLevel *bb = book.BestBid();
        const PriceLevel *ba = book.BestAsk();
        out_ << std::left << std::setw(8) << t << std::right;
        if (bb)
            out_ << std::setw(10) << bb->TotalQuantity() << COLORS::green
                 << std::setw(10) << bb->price << COLORS::reset;
        else
            out_ << std::setw(10) << "-" << std::setw(10) << "-";
        if (ba)
            out_ << COLORS::red << std::setw(10) << ba->price << COLORS::reset
                 << std::setw(10) << ba->TotalQuantity();
        else
            out_ << std::setw(10) << "-" << std::setw(10) << "-";
        if (bb && ba)
            out_ << std::setw(8) << (ba->price - bb->price);
        else
            out_ << std::setw(8) << "-";
        out_ << "\n";
    }
    return true;
}

bool Shell::cmd_order_info(const Args &a) {
    if (!need_inspect())
        return true;
    uint64_t id;
    if (a.size() != 2 || !parse_u64(a[1], id))
        return fail("usage: order ID");
    Exchange &ex = *s_.exchange();
    auto sym = ex.SymbolOfOrder(id);
    if (!sym)
        return fail("order " + a[1] + " is not on any book");
    const OrderNode *n = ex.GetBook(*sym).FindOrder(id);
    if (!n)
        return fail("order " + a[1] + " is not on any book");
    // Queue position: orders ahead at the same level
    int ahead = 0;
    for (const OrderNode *p = n->prev; p; p = p->prev)
        ahead++;
    out_ << "order " << id << ": " << side_name(n->side) << " " << n->quantity
         << " " << ex.TickerOf(*sym) << " @ " << n->price << ", shard "
         << (int)shard_of(id) << ", " << ahead << " ahead in queue, level qty "
         << n->level->TotalQuantity() << "\n";
    return true;
}

void Shell::print_events(std::size_t last_n) {
    auto *local = dynamic_cast<LocalSession *>(&s_);
    if (!local)
        return;
    std::vector<OutBoundEvent> evs = local->TakeEvents();
    std::size_t start = (last_n && evs.size() > last_n) ? evs.size() - last_n : 0;
    for (std::size_t i = start; i < evs.size(); i++)
        out_ << "  " << COLORS::dim << evs[i] << COLORS::reset << "\n";
}

bool Shell::cmd_events(const Args &a) {
    if (!need_inspect())
        return true;
    uint64_t n = 0;
    if (a.size() > 1 && !parse_u64(a[1], n))
        return fail("usage: events [N]");
    print_events(n);
    return true;
}

bool Shell::cmd_flow(const Args &a) {
    // flow SYM N [SEED]: synthetic flow in the style of exchange_server
    if (!need_inspect())
        return true;
    uint64_t n, seed = 42;
    if (a.size() < 3 || !parse_u64(a[2], n) || (a.size() > 3 && !parse_u64(a[3], seed)))
        return fail("usage: flow SYM N [SEED]");
    Exchange &ex = *s_.exchange();
    auto sym = ex.ResolveSymbol(upper(a[1]));
    if (!sym)
        return fail("unknown symbol '" + a[1] + "'");

    uint64_t x = seed ? seed : 1;
    auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    // Anchor the mid on the current book if it has one
    uint64_t mid = 10000;
    const OrderBook &book = ex.GetBook(*sym);
    if (book.BestBid() && book.BestAsk())
        mid = (book.BestBid()->price + book.BestAsk()->price) / 2;
    else if (book.BestBid())
        mid = book.BestBid()->price + 5;
    else if (book.BestAsk())
        mid = book.BestAsk()->price > 5 ? book.BestAsk()->price - 5 : 1;

    std::vector<OrderId> live;
    uint64_t submitted = 0, cancels = 0, modifies = 0, fills = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (rnd() % 20 == 0)
            mid += (rnd() % 3) - 1;
        unsigned r = rnd() % 100;
        if (r < 55 || live.empty()) {
            Side side = rnd() % 2 ? Side::Buy : Side::Sell;
            uint64_t off = 1 + rnd() % 15;
            Price px = side == Side::Buy ? (mid > off ? mid - off : 1) : mid + off;
            auto res = ex.SubmitOrder(*sym, px, 1 + rnd() % 500, side);
            submitted++;
            if (res.resting) live.push_back(res.order_id);
            if (res.qty_executed) fills++;
        } else if (r < 80) {
            std::size_t k = rnd() % live.size();
            ex.CancelOrder(live[k]);
            live[k] = live.back(); live.pop_back();
            cancels++;
        } else if (r < 90) {
            ex.ModifyOrder(live[rnd() % live.size()], 1 + rnd() % 50);
            modifies++;
        } else {
            Side side = rnd() % 2 ? Side::Buy : Side::Sell;
            Price px = side == Side::Buy ? mid + 20 : (mid > 20 ? mid - 20 : 1);
            auto res = ex.SubmitOrder(*sym, px, 1 + rnd() % 800, side,
                                      OrderType::LIMIT, TypeInForce::IOC);
            submitted++;
            if (res.qty_executed) fills++;
        }
    }
    // Discard the flood of events and fills this generated
    static_cast<LocalSession &>(s_).TakeEvents();
    s_.TakeFills();
    out_ << "flow " << upper(a[1]) << ": " << submitted << " orders, "
         << cancels << " cancels, " << modifies << " modifies, " << fills
         << " orders filled, " << book.orderCount() << " resting\n";
    return true;
}

bool Shell::cmd_symbols() {
    Exchange *ex = s_.exchange();
    for (const std::string &t : s_.Tickers()) {
        out_ << t;
        if (ex)
            out_ << "  id " << *ex->ResolveSymbol(t) << "  shard "
                 << (int)ex->ShardOf(*ex->ResolveSymbol(t));
        out_ << "\n";
    }
    return true;
}
