#pragma once
// The synthetic market model behind both flow front-ends: the in-process
// "flowgen" ingress adapter and the TCP load tool. It decides *what* to do
// (a new order, a cancel, a reduction) and learns from the reports it is
// told about; the front-end decides how to send it and how fast.
//
// Per symbol: a mean-reverting fundamental with rare jumps and its own
// volatility; passive quotes at geometric offsets from the mid with
// round-lot fat-tailed sizes; momentum-biased aggressive IOCs with
// occasional multi-level sweeps. Per model: quiet/normal/burst regimes
// with sticky durations. Fills anchor the mid to real prints.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace flow {

struct NewOrder {
    std::size_t sym;
    bool buy;
    std::uint32_t qty;
    std::uint64_t px;
    bool ioc;
};
struct Cancel {
    std::uint64_t cl;
};
struct Reduce {
    std::uint64_t cl;
    std::uint32_t qty;
};
struct Action {
    enum Kind { kNew, kCancel, kReduce } kind;
    NewOrder new_order{};
    Cancel cancel{};
    Reduce reduce{};
};

class Model {
  public:
    Model(std::size_t num_symbols, const std::vector<std::string> &tickers, std::uint64_t seed)
        : rng_(seed) {
        mid_.resize(num_symbols);
        fund_.resize(num_symbols);
        vol_.resize(num_symbols);
        trend_.assign(num_symbols, 0.0);
        for (std::size_t i = 0; i < num_symbols; ++i) {
            const std::string &t = i < tickers.size() ? tickers[i] : std::to_string(i);
            std::uint64_t base = 2000 + (hash(t) % 60) * 500; // 2,000 .. 31,500
            mid_[i] = base;
            fund_[i] = static_cast<double>(base);
            vol_[i] = 0.0004 + (hash(t + "v") % 10) * 0.0002;
        }
    }

    /// Activity multiplier for pacing: a sticky regime per model.
    double activity(std::uint64_t now_ns) {
        if (now_ns >= regime_until_) {
            unsigned r = rng_() % 100;
            activity_ = r < 25 ? 0.4 : r < 85 ? 1.0 : 4.0;
            double mean_s = activity_ > 1.5 ? 1.5 : activity_ < 0.5 ? 6.0 : 10.0;
            std::exponential_distribution<double> ex(1.0 / mean_s);
            regime_until_ = now_ns + static_cast<std::uint64_t>(ex(rng_) * 1e9) + 200'000'000ull;
        }
        return activity_;
    }

    /// One step of the mix. Returns the actions to send (0..2).
    std::vector<Action> step() {
        std::vector<Action> out;
        std::size_t si = rng_() % mid_.size();
        std::uint64_t &mid = mid_[si];
        double &f = fund_[si];
        std::normal_distribution<double> z(0.0, 1.0);
        f += f * vol_[si] * z(rng_) * 0.3;
        if (rng_() % 4000 == 0)
            f *= 1.0 + (rng_() % 2 ? 1 : -1) * (0.004 + (rng_() % 10) * 0.001);
        double gap = f - static_cast<double>(mid);
        if (std::fabs(gap) > 1.0 && rng_() % 3 == 0)
            mid = static_cast<std::uint64_t>(std::max(10.0, static_cast<double>(mid) + (gap > 0 ? 1 : -1)));
        trend_[si] = 0.9 * trend_[si] + 0.1 * (gap > 0 ? 1.0 : gap < 0 ? -1.0 : 0.0);

        unsigned r = rng_() % 100;
        if (r < 52) {
            bool buy = rng_() % 2;
            std::uint64_t off = offset();
            std::uint64_t px = buy ? (mid > off ? mid - off : 1) : mid + off;
            out.push_back(Action{Action::kNew, NewOrder{si, buy, size(1.0), px, false}, {}, {}});
        } else if (r < 78 && !live_vec_.empty()) {
            out.push_back(Action{Action::kCancel, {}, Cancel{live_vec_[rng_() % live_vec_.size()]}, {}});
        } else if (r < 86 && !live_vec_.empty()) {
            std::uint64_t cl = live_vec_[rng_() % live_vec_.size()];
            auto it = live_.find(cl);
            if (it != live_.end() && it->second.leaves > 1)
                out.push_back(Action{Action::kReduce, {}, {}, Reduce{cl, 1 + static_cast<std::uint32_t>(rng_() % it->second.leaves)}});
        } else {
            double p_buy = 0.5 + 0.25 * trend_[si];
            bool buy = (rng_() % 1000) < p_buy * 1000;
            bool sweep = rng_() % 400 == 0;
            std::uint64_t reach = sweep ? 15 : 1 + rng_() % 3;
            std::uint64_t px = buy ? mid + reach : (mid > reach ? mid - reach : 1);
            out.push_back(Action{Action::kNew, NewOrder{si, buy, size(sweep ? 12.0 : 1.4), px, true}, {}, {}});
        }
        if (live_vec_.size() > max_live_)
            out.push_back(Action{Action::kCancel, {}, Cancel{live_vec_[rng_() % live_vec_.size()]}, {}});
        return out;
    }

    // ---- what the front-end tells the model about its orders ----
    /// A new order was sent under client id `cl` for symbol `sym`.
    void sent(std::uint64_t cl, std::size_t sym) { pending_[cl] = sym; }
    /// Accepted with `leaves` resting (0 if it filled or was IOC-dropped).
    void accepted(std::uint64_t cl, std::uint32_t leaves) {
        auto p = pending_.find(cl);
        if (p == pending_.end())
            return;
        std::size_t sym = p->second;
        pending_.erase(p);
        if (leaves > 0)
            add_live(cl, Live{sym, leaves});
    }
    void rejected(std::uint64_t cl) { pending_.erase(cl); }
    void cancelled(std::uint64_t cl) {
        remove_live(cl);
        pending_.erase(cl);
    }
    void modified(std::uint64_t cl, std::uint32_t leaves) {
        auto it = live_.find(cl);
        if (it != live_.end())
            it->second.leaves = leaves;
    }
    void executed(std::uint64_t cl, std::uint64_t px, std::uint32_t leaves) {
        auto it = live_.find(cl);
        if (it != live_.end()) {
            mid_[it->second.sym] = px; // anchor to prints
            if (leaves == 0)
                remove_live(cl);
            else
                it->second.leaves = leaves;
        } else if (auto p = pending_.find(cl); p != pending_.end()) {
            mid_[p->second] = px;
        }
    }
    std::size_t live() const { return live_.size(); }
    void set_max_live(std::size_t n) { max_live_ = n; }

  private:
    struct Live {
        std::size_t sym;
        std::uint32_t leaves;
    };
    static std::uint64_t hash(const std::string &s) {
        std::uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }
    std::uint64_t offset() {
        std::uint64_t k = 1;
        while (k < 40 && rng_() % 100 < 62)
            ++k;
        return k;
    }
    std::uint32_t size(double scale) {
        std::normal_distribution<double> n(4.0, 0.9);
        double v = std::exp(n(rng_)) * scale;
        std::uint32_t q = static_cast<std::uint32_t>(std::max(1.0, std::min(20000.0, v)));
        return q >= 100 ? (q / 100) * 100 : q;
    }
    void add_live(std::uint64_t cl, Live l) {
        pos_[cl] = live_vec_.size();
        live_vec_.push_back(cl);
        live_[cl] = l;
    }
    void remove_live(std::uint64_t cl) {
        auto it = live_.find(cl);
        if (it == live_.end())
            return;
        std::size_t p = pos_[cl];
        if (p + 1 != live_vec_.size()) {
            live_vec_[p] = live_vec_.back();
            pos_[live_vec_[p]] = p;
        }
        live_vec_.pop_back();
        pos_.erase(cl);
        live_.erase(it);
    }

    std::mt19937_64 rng_;
    std::vector<std::uint64_t> mid_;
    std::vector<double> fund_, vol_, trend_;
    double activity_ = 1.0;
    std::uint64_t regime_until_ = 0;
    std::size_t max_live_ = 5000;
    std::unordered_map<std::uint64_t, std::size_t> pending_; // cl -> sym
    std::unordered_map<std::uint64_t, Live> live_;
    std::unordered_map<std::uint64_t, std::size_t> pos_;
    std::vector<std::uint64_t> live_vec_;
};

} // namespace flow
