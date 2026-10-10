// Built-in "flowgen" ingress adapter: the synthetic market model driving
// the exchange in-process through the ingress API, so a lone
// exchange_server is a live market. Config:
//   {"type":"flowgen","profile":"calm|busy|load","sessions":2,"rate":0,"seed":1}
// rate (orders/s total) overrides the profile when > 0.
#include "flowgen_ingress.hpp"
#include "flow/model.hpp"
#include "nlohmann/json.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::uint64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Session {
    mktsim_session *xs = nullptr;
    std::unique_ptr<flow::Model> model;
    std::thread thread;
    std::uint64_t next_cl = 1;
    // request id -> client id; order id -> client id
    std::unordered_map<std::uint64_t, std::uint64_t> by_req, by_order;
    std::unordered_map<std::uint64_t, std::uint64_t> order_of; // cl -> order id
};

struct State {
    const mktsim_exchange_api *api;
    mktsim_exchange *ex;
    std::vector<std::string> tickers;
    std::vector<std::uint32_t> ids;
    int sessions = 2;
    double rate = 0;
    std::string profile = "calm";
    std::uint64_t seed = 1;
    std::vector<std::unique_ptr<Session>> sess;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> sent{0}, execs{0};

    void run(Session &s, double per_sec) {
        const double ns_per_order = per_sec > 0 ? 1e9 / per_sec : 0;
        double budget = 0;
        std::uint64_t last = mono_ns();
        while (running.load(std::memory_order_relaxed)) {
            const std::uint64_t now = mono_ns();
            const double pace = ns_per_order / s.model->activity(now);
            int burst = 0;
            while (budget <= 0.0 && burst < 64) {
                for (const flow::Action &a : s.model->step())
                    send(s, a);
                ++burst;
                budget += pace;
            }
            budget -= double(now - last);
            last = now;
            api->poll(s.xs, &State::on_report, &s, 1024);
            if (budget > 0.5e6)
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long>(std::min(budget / 1000.0, 20000.0))));
        }
    }

    void send(Session &s, const flow::Action &a) {
        mktsim_order_req r{};
        std::uint64_t cl = 0;
        switch (a.kind) {
        case flow::Action::kNew: {
            cl = s.next_cl++;
            r.kind = MKTSIM_REQ_NEW;
            r.side = a.new_order.buy ? MKTSIM_BUY : MKTSIM_SELL;
            r.ord_type = MKTSIM_LIMIT;
            r.tif = a.new_order.ioc ? MKTSIM_IOC : MKTSIM_GTC;
            r.symbol_id = ids[a.new_order.sym];
            r.qty = a.new_order.qty;
            r.price = a.new_order.px;
            s.model->sent(cl, a.new_order.sym);
            break;
        }
        case flow::Action::kCancel: {
            cl = a.cancel.cl;
            auto it = s.order_of.find(cl);
            if (it == s.order_of.end())
                return;
            r.kind = MKTSIM_REQ_CANCEL;
            r.order_id = it->second;
            break;
        }
        case flow::Action::kReduce: {
            cl = a.reduce.cl;
            auto it = s.order_of.find(cl);
            if (it == s.order_of.end())
                return;
            r.kind = MKTSIM_REQ_MODIFY;
            r.order_id = it->second;
            r.qty = a.reduce.qty;
            r.price = 0;
            break;
        }
        }
        r.request_id = cl;
        if (api->submit(s.xs, &r) == MKTSIM_OK) {
            if (a.kind == flow::Action::kNew)
                s.by_req[cl] = cl;
            sent.fetch_add(1, std::memory_order_relaxed);
        } else if (a.kind == flow::Action::kNew) {
            s.model->rejected(cl);
        }
    }

    static void on_report(void *user, const mktsim_report *r) {
        auto &s = *static_cast<Session *>(user);
        std::uint64_t cl = 0;
        if (r->request_id) {
            cl = r->request_id;
        } else if (auto it = s.by_order.find(r->order_id); it != s.by_order.end()) {
            cl = it->second;
        } else {
            return;
        }
        switch (r->kind) {
        case MKTSIM_RPT_ACCEPTED:
            s.by_order[r->order_id] = cl;
            s.order_of[cl] = r->order_id;
            s.model->accepted(cl, r->leaves_qty);
            if (r->leaves_qty == 0) {
                s.order_of.erase(cl);
            }
            break;
        case MKTSIM_RPT_REJECTED:
            s.model->rejected(cl);
            break;
        case MKTSIM_RPT_CANCELLED:
            s.model->cancelled(cl);
            s.order_of.erase(cl);
            s.by_order.erase(r->order_id);
            break;
        case MKTSIM_RPT_MODIFIED:
            s.model->modified(cl, r->leaves_qty);
            break;
        case MKTSIM_RPT_EXECUTION:
            s.model->executed(cl, r->price, r->leaves_qty);
            if (r->leaves_qty == 0) {
                s.order_of.erase(cl);
                s.by_order.erase(r->order_id);
            }
            break;
        default:
            break;
        }
    }
};

int fg_init(const mktsim_exchange_api *api, mktsim_exchange *ex, const char *config_json, void **state) {
    if (api->version != MKTSIM_INGRESS_API_VERSION)
        return 1;
    auto *st = new State{api, ex};
    try {
        nlohmann::json j = nlohmann::json::parse(config_json && *config_json ? config_json : "{}");
        st->sessions = std::max(1, j.value("sessions", 2));
        st->rate = j.value("rate", 0.0);
        st->profile = j.value("profile", std::string("calm"));
        st->seed = j.value("seed", 1ull);
    } catch (const std::exception &e) {
        api->log(ex, "flowgen", (std::string("bad config: ") + e.what()).c_str());
        delete st;
        return 1;
    }
    std::vector<mktsim_symbol_info> info(4096);
    std::size_t n = api->symbols(ex, info.data(), info.size());
    for (std::size_t i = 0; i < n; ++i) {
        st->ids.push_back(info[i].symbol_id);
        st->tickers.emplace_back(info[i].ticker);
    }
    if (st->rate <= 0) {
        const double k = static_cast<double>(st->ids.size());
        st->rate = st->profile == "busy" ? 100.0 * k : st->profile == "load" ? 20000.0 : 20.0 * k;
    }
    *state = st;
    return 0;
}

int fg_start(void *state) {
    auto *st = static_cast<State *>(state);
    st->running.store(true);
    for (int i = 0; i < st->sessions; ++i) {
        auto s = std::make_unique<Session>();
        s->xs = st->api->open_session(st->ex, "flowgen");
        if (!s->xs)
            return 1;
        s->model = std::make_unique<flow::Model>(st->ids.size(), st->tickers, st->seed * 7919 + i);
        s->model->set_max_live(5000);
        Session *sp = s.get();
        const double per = st->rate / st->sessions;
        s->thread = std::thread([st, sp, per] { st->run(*sp, per); });
        st->sess.push_back(std::move(s));
    }
    char msg[160];
    std::snprintf(msg, sizeof(msg), "%d session(s), profile %s, %.0f orders/s over %zu symbols", st->sessions,
                  st->profile.c_str(), st->rate, st->ids.size());
    st->api->log(st->ex, "flowgen", msg);
    return 0;
}

void fg_stop(void *state) {
    auto *st = static_cast<State *>(state);
    if (!st->running.exchange(false))
        return;
    for (auto &s : st->sess) {
        if (s->thread.joinable())
            s->thread.join();
        if (s->xs)
            st->api->close_session(s->xs);
        s->xs = nullptr;
    }
}

void fg_destroy(void *state) { delete static_cast<State *>(state); }

} // namespace

IngressPlugin::Entry flowgen_ingress_entry() { return IngressPlugin::Entry{&fg_init, &fg_start, &fg_stop, &fg_destroy}; }
